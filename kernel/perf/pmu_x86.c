/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The hardware performance-monitoring unit, as Intel's architectural
 * specification defines it (CPUID leaf 0x0A) and as everything since Core 2 —
 * and KVM's virtual PMU — implements it.
 *
 * WHY THIS EXISTS
 *
 * perf_event_open(2) without hardware counters answers the easy half of the
 * question. `perf record -e cycles`, `perf stat` and every profile that says
 * where the time really went need a counter the CPU increments itself: cycles,
 * instructions, cache misses, branch mispredicts. Software counters cannot
 * stand in for those, and reporting zero for them would be worse than the
 * -EOPNOTSUPP this replaces.
 *
 * WHAT IT PROGRAMS
 *
 * The general-purpose counters only: IA32_PERFEVTSELx selects an event,
 * IA32_PMCx counts it. The fixed counters are deliberately left alone --
 * they can only count three specific events, the general ones can count those
 * three as well, and a machine with four general counters has more of them
 * than this kernel has events to put in them.
 *
 * HOW A COUNT IS ATTRIBUTED
 *
 * A hardware counter counts whatever the CPU is doing, so a per-task event has
 * to be told when its task is running. Every counter is read at each context
 * switch and at each timer tick, and the delta since the last read is credited
 * to the task that was running over that interval -- which is exact at the
 * switch boundaries that matter, because within one timeslice there is only
 * one task to credit.
 *
 * A counter is programmed by the CPU that owns it, from its own tick, never by
 * an IPI: MSR writes to another core's PMU are not possible, and the tick is
 * a millisecond away. A system-wide event therefore starts counting on every
 * CPU within one tick of being enabled, and each CPU adds its own deltas into
 * the event's total.
 *
 * WHAT IS REFUSED
 *
 * A machine whose CPUID says there is no architectural PMU (every TCG guest,
 * and a KVM guest whose host did not give it one) reports none, and
 * perf_event_open keeps answering -EOPNOTSUPP for hardware events. That is the
 * honest answer: a counter wired to a register that reads zero for ever would
 * make `perf` print a profile of nothing at all.
 */
#include <b1nix/errno.h>
#include <b1nix/console.h>
#include <b1nix/kprintf.h>
#include <b1nix/lapic.h>
#include <b1nix/memtype.h>
#include <b1nix/perf_event.h>
#include <b1nix/perf_event_abi.h>
#include <b1nix/sched.h>

#include <string.h>

#if defined(__x86_64__)

/* ── the MSRs ────────────────────────────────────────────────────────────── */

#define MSR_IA32_PERFEVTSEL0 0x186u
#define MSR_IA32_PMC0 0x0C1u
#define MSR_IA32_FIXED_CTR0 0x309u
#define MSR_IA32_FIXED_CTR_CTRL 0x38Du
#define MSR_IA32_PERF_GLOBAL_CTRL 0x38Fu
#define MSR_IA32_PERF_GLOBAL_OVF_CTRL 0x390u

/* IA32_PERFEVTSELx */
#define EVTSEL_USR (1ull << 16)
#define EVTSEL_OS (1ull << 17)
#define EVTSEL_EN (1ull << 22)

/* Architectural events, as (umask << 8) | event -- the same packing
 * IA32_PERFEVTSELx uses for its low sixteen bits, which is also what
 * PERF_TYPE_RAW's config carries. */
#define EV_CPU_CYCLES 0x003Cu
#define EV_INSTRUCTIONS 0x00C0u
#define EV_REF_CYCLES 0x013Cu
#define EV_LLC_REFERENCES 0x4F2Eu
#define EV_LLC_MISSES 0x412Eu
#define EV_BRANCH_INSNS 0x00C4u
#define EV_BRANCH_MISSES 0x00C5u

/* Bit in CPUID.0AH:EBX that says an architectural event is NOT available. */
#define ARCH_EV_UNAVAIL_CYCLES (1u << 0)
#define ARCH_EV_UNAVAIL_INSTRUCTIONS (1u << 1)
#define ARCH_EV_UNAVAIL_REF_CYCLES (1u << 2)
#define ARCH_EV_UNAVAIL_LLC_REF (1u << 3)
#define ARCH_EV_UNAVAIL_LLC_MISS (1u << 4)
#define ARCH_EV_UNAVAIL_BRANCH (1u << 5)
#define ARCH_EV_UNAVAIL_BRANCH_MISS (1u << 6)

/* How many events may be counted in hardware at once, machine-wide. Bounded by
 * the counters the CPU has; four is what every architectural PMU since Core 2
 * offers with hyper-threading off, two with it on. */
#define PMU_MAX_SLOTS 8

/* The three fixed-function counters, in the order IA32_FIXED_CTRx has them.
 * They are not a shortcut: on a KVM guest the general-purpose counters may not
 * be backed for every architectural event -- unhalted core cycles reads as
 * almost nothing through IA32_PMCx there, while IA32_FIXED_CTR1 counts it
 * exactly. Linux prefers the fixed counters for these three for the same
 * reason, and they cost nothing: using one leaves a general counter free. */
#define PMU_FIXED_INSTRUCTIONS 0
#define PMU_FIXED_CPU_CYCLES 1
#define PMU_FIXED_REF_CYCLES 2
#define PMU_FIXED_COUNT 3

struct pmu_slot {
  int in_use;
  u64 evsel;      /* the low sixteen bits of IA32_PERFEVTSELx */
  usize target;   /* pid, or 0 for every task on the machine */
  int user, kernel; /* which privilege levels this event counts */
  int fixed;      /* the fixed counter this uses, or -1 for a general one */
  int hw;         /* index of the hardware counter, fixed or general */
  u64 count;      /* accumulated, all CPUs */
};

/* Per-CPU: what each slot's counter read last time this CPU looked. */
struct pmu_cpu {
  u64 last[PMU_MAX_SLOTS];
  int live[PMU_MAX_SLOTS];  /* this CPU has this slot's counter programmed */
  u32 gen;                  /* the configuration this CPU has programmed */
};

static struct pmu_slot g_slots[PMU_MAX_SLOTS];
static struct pmu_cpu g_cpu[MAX_CPUS];
static volatile u32 g_gen = 1; /* bumped whenever the slot table changes */

static int g_pmu_version;
static int g_pmu_counters;   /* general-purpose counters per CPU */
static int g_pmu_fixed;      /* fixed-function counters per CPU */
static u64 g_pmu_mask;       /* counter width as a mask, for wraparound */
static u32 g_pmu_unavail;    /* CPUID.0AH:EBX */
static int g_pmu_ready;

static inline void cpuid_count(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c,
                               u32 *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(sub));
}

void perf_pmu_init(void) {
  u32 a, b, c, d;

  cpuid_count(0, 0, &a, &b, &c, &d);
  if (a < 0x0A)
    return; /* the leaf that describes the PMU does not exist */

  cpuid_count(0x0A, 0, &a, &b, &c, &d);
  g_pmu_version = (int)(a & 0xFF);
  g_pmu_counters = (int)((a >> 8) & 0xFF);
  int width = (int)((a >> 16) & 0xFF);

  g_pmu_unavail = b;
  if (g_pmu_version == 0 || g_pmu_counters == 0 || width == 0 || width > 64)
    return;
  if (g_pmu_counters > PMU_MAX_SLOTS)
    g_pmu_counters = PMU_MAX_SLOTS;
  g_pmu_mask = (width >= 64) ? ~0ull : ((1ull << width) - 1);
  /* EDX bits 4:0 count the fixed counters; they only exist from version 2. */
  g_pmu_fixed = (g_pmu_version >= 2) ? (int)(d & 0x1F) : 0;
  if (g_pmu_fixed > PMU_FIXED_COUNT)
    g_pmu_fixed = PMU_FIXED_COUNT;

  g_pmu_ready = 1;
  k_info("perf", "PMU v%d, %d general + %d fixed counters, %d bits wide",
         g_pmu_version, g_pmu_counters, g_pmu_fixed, width);
}

int perf_pmu_available(void) { return g_pmu_ready; }

/* ── what an attr asks for, in PMU terms ─────────────────────────────────── */

/* Is this architectural event present on this CPU? */
static int pmu_event_available(u64 evsel) {
  u32 bit = 0;

  switch (evsel) {
  case EV_CPU_CYCLES: bit = ARCH_EV_UNAVAIL_CYCLES; break;
  case EV_INSTRUCTIONS: bit = ARCH_EV_UNAVAIL_INSTRUCTIONS; break;
  case EV_REF_CYCLES: bit = ARCH_EV_UNAVAIL_REF_CYCLES; break;
  case EV_LLC_REFERENCES: bit = ARCH_EV_UNAVAIL_LLC_REF; break;
  case EV_LLC_MISSES: bit = ARCH_EV_UNAVAIL_LLC_MISS; break;
  case EV_BRANCH_INSNS: bit = ARCH_EV_UNAVAIL_BRANCH; break;
  case EV_BRANCH_MISSES: bit = ARCH_EV_UNAVAIL_BRANCH_MISS; break;
  default: return 1; /* a raw event: the caller knows its own CPU */
  }
  return (g_pmu_unavail & bit) == 0;
}

/* PERF_TYPE_HW_CACHE packs (id | op << 8 | result << 16). Only the pairs an
 * architectural PMU really has an event for are answered; the rest are
 * refused, because "L1 data cache misses" counted with an LLC event would be
 * a number that looks right and is not. */
static int pmu_cache_event(u64 config, u64 *out) {
  u64 id = config & 0xFF, op = (config >> 8) & 0xFF, res = (config >> 16) & 0xFF;

  if (id == PERF_COUNT_HW_CACHE_LL && op == PERF_COUNT_HW_CACHE_OP_READ) {
    *out = (res == PERF_COUNT_HW_CACHE_RESULT_MISS) ? EV_LLC_MISSES
                                                    : EV_LLC_REFERENCES;
    return 0;
  }
  if (id == PERF_COUNT_HW_CACHE_BPU && op == PERF_COUNT_HW_CACHE_OP_READ) {
    *out = (res == PERF_COUNT_HW_CACHE_RESULT_MISS) ? EV_BRANCH_MISSES
                                                    : EV_BRANCH_INSNS;
    return 0;
  }
  return -EOPNOTSUPP;
}

int perf_pmu_map(const struct perf_event_attr *a, u64 *evsel_out) {
  u64 ev;

  if (!g_pmu_ready)
    return -EOPNOTSUPP;

  switch (a->type) {
  case PERF_TYPE_HARDWARE:
    switch (a->config) {
    case PERF_COUNT_HW_CPU_CYCLES: ev = EV_CPU_CYCLES; break;
    case PERF_COUNT_HW_INSTRUCTIONS: ev = EV_INSTRUCTIONS; break;
    case PERF_COUNT_HW_REF_CPU_CYCLES: ev = EV_REF_CYCLES; break;
    case PERF_COUNT_HW_CACHE_REFERENCES: ev = EV_LLC_REFERENCES; break;
    case PERF_COUNT_HW_CACHE_MISSES: ev = EV_LLC_MISSES; break;
    case PERF_COUNT_HW_BRANCH_INSTRUCTIONS: ev = EV_BRANCH_INSNS; break;
    case PERF_COUNT_HW_BRANCH_MISSES: ev = EV_BRANCH_MISSES; break;
    default:
      /* Bus cycles, stalled cycles front/back: no architectural event, and
       * the model-specific ones differ per microarchitecture. */
      return -EOPNOTSUPP;
    }
    break;
  case PERF_TYPE_HW_CACHE:
    if (pmu_cache_event(a->config, &ev) < 0)
      return -EOPNOTSUPP;
    break;
  case PERF_TYPE_RAW:
    ev = a->config & 0xFFFFu;
    if (!ev)
      return -EINVAL;
    break;
  default:
    return -EOPNOTSUPP;
  }
  if (!pmu_event_available(ev))
    return -EOPNOTSUPP;
  *evsel_out = ev;
  return 0;
}

/* ── slots ───────────────────────────────────────────────────────────────── */

/* Which fixed counter counts this event, or -1. */
static int pmu_fixed_for(u64 evsel) {
  switch (evsel) {
  case EV_INSTRUCTIONS: return PMU_FIXED_INSTRUCTIONS;
  case EV_CPU_CYCLES: return PMU_FIXED_CPU_CYCLES;
  case EV_REF_CYCLES: return PMU_FIXED_REF_CYCLES;
  default: return -1;
  }
}

/* Is that fixed counter already taken by another event? Two events on one
 * counter would need multiplexing, which this does not do. */
static int pmu_fixed_taken(int fixed) {
  for (int i = 0; i < PMU_MAX_SLOTS; i++)
    if (g_slots[i].in_use && g_slots[i].fixed == fixed)
      return 1;
  return 0;
}

/* A general-purpose counter nothing is using, or -1.
 *
 * Counting the busy ones and using the count as the index is not the same
 * thing: free the first of two events and open a third, and the count picks
 * the index the second one is already programmed on. Two slots then read the
 * same MSR, each subtracting the other's progress, and both report a fraction
 * of the truth. */
static int pmu_general_free(void) {
  for (int hw = 0; hw < g_pmu_counters; hw++) {
    int taken = 0;

    for (int i = 0; i < PMU_MAX_SLOTS && !taken; i++)
      if (g_slots[i].in_use && g_slots[i].fixed < 0 && g_slots[i].hw == hw)
        taken = 1;
    if (!taken)
      return hw;
  }
  return -1;
}

int perf_pmu_slot_alloc(u64 evsel, usize target, int user, int kernel) {
  if (!g_pmu_ready)
    return -EOPNOTSUPP;

  int fixed = pmu_fixed_for(evsel);

  if (fixed >= g_pmu_fixed || (fixed >= 0 && pmu_fixed_taken(fixed)))
    fixed = -1; /* no such fixed counter here, or it is busy: use a general one */

  int hw = fixed >= 0 ? fixed : pmu_general_free();

  if (hw < 0)
    return -EBUSY;

  for (int i = 0; i < PMU_MAX_SLOTS; i++) {
    if (g_slots[i].in_use)
      continue;
    g_slots[i].evsel = evsel;
    g_slots[i].target = target;
    g_slots[i].user = user;
    g_slots[i].kernel = kernel;
    g_slots[i].fixed = fixed;
    g_slots[i].hw = hw;
    g_slots[i].count = 0;
    g_slots[i].in_use = 1;
    __atomic_add_fetch(&g_gen, 1, __ATOMIC_ACQ_REL);
    return i;
  }
  /* Every counter is taken. Linux multiplexes here and scales the result;
   * this says no instead, because a scaled count that nothing labels as
   * scaled is a number the caller will read as exact. */
  return -EBUSY;
}

void perf_pmu_slot_free(int slot) {
  if (slot < 0 || slot >= PMU_MAX_SLOTS || !g_slots[slot].in_use)
    return;
  g_slots[slot].in_use = 0;
  __atomic_add_fetch(&g_gen, 1, __ATOMIC_ACQ_REL);
}

u64 perf_pmu_slot_count(int slot) {
  if (slot < 0 || slot >= PMU_MAX_SLOTS)
    return 0;
  return __atomic_load_n(&g_slots[slot].count, __ATOMIC_RELAXED);
}

/* ── the hardware, on the CPU that owns it ───────────────────────────────── */

static struct pmu_cpu *pmu_this_cpu(void) {
  struct percpu *p = get_percpu();
  unsigned cpu = p ? (unsigned)p->cpu_id : 0;

  return &g_cpu[cpu < MAX_CPUS ? cpu : 0];
}

/* Program this CPU's counters to match the slot table. */
static void pmu_reprogram(struct pmu_cpu *pc) {
  u64 enable_mask = 0;
  u64 fixed_ctrl = 0;

  /* Every general counter off first: a slot that has gone away must stop
   * counting, and a counter whose event changed must not carry the old one's
   * value into the new one. */
  for (int i = 0; i < g_pmu_counters; i++)
    wrmsr(MSR_IA32_PERFEVTSEL0 + (u32)i, 0);

  for (int i = 0; i < PMU_MAX_SLOTS; i++) {
    pc->live[i] = 0;
    if (!g_slots[i].in_use)
      continue;

    int hw = g_slots[i].hw;

    if (g_slots[i].fixed >= 0) {
      /* Four bits per fixed counter: OS, USR, AnyThread, PMI. */
      u64 nib = 0;

      if (g_slots[i].kernel)
        nib |= 1ull;
      if (g_slots[i].user)
        nib |= 2ull;
      fixed_ctrl |= nib << (4 * hw);
      wrmsr(MSR_IA32_FIXED_CTR0 + (u32)hw, 0);
      enable_mask |= 1ull << (32 + hw);
    } else {
      u64 sel = g_slots[i].evsel | EVTSEL_EN;

      if (g_slots[i].user)
        sel |= EVTSEL_USR;
      if (g_slots[i].kernel)
        sel |= EVTSEL_OS;
      wrmsr(MSR_IA32_PMC0 + (u32)hw, 0);
      wrmsr(MSR_IA32_PERFEVTSEL0 + (u32)hw, sel);
      enable_mask |= 1ull << hw;
    }
    pc->live[i] = 1;
    pc->last[i] = 0;
  }
  if (g_pmu_fixed)
    wrmsr(MSR_IA32_FIXED_CTR_CTRL, fixed_ctrl);
  /* Version 2 added a global switch that gates every counter; without it set,
   * a perfectly programmed PERFEVTSEL counts nothing. */
  if (g_pmu_version >= 2)
    wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, enable_mask);
  pc->gen = __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE);
}

/* Read every counter this CPU runs and credit the delta to `pid` -- the task
 * that was running since the last read. Called from the tick and from the
 * context switch, both on this CPU with interrupts off. */
static void pmu_accumulate(usize pid) {
  struct pmu_cpu *pc = pmu_this_cpu();

  if (!g_pmu_ready)
    return;
  if (pc->gen != __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE)) {
    pmu_reprogram(pc);
    return; /* the counters were just zeroed: nothing to credit yet */
  }
  for (int i = 0; i < PMU_MAX_SLOTS; i++) {
    if (!pc->live[i] || !g_slots[i].in_use)
      continue;

    u32 msr = g_slots[i].fixed >= 0 ? MSR_IA32_FIXED_CTR0 + (u32)g_slots[i].hw
                                    : MSR_IA32_PMC0 + (u32)g_slots[i].hw;
    u64 now = rdmsr(msr) & g_pmu_mask;
    u64 delta = (now - pc->last[i]) & g_pmu_mask;

    pc->last[i] = now;
    if (!delta)
      continue;
    /* A per-task event is credited only for the intervals its task ran. */
    if (g_slots[i].target && g_slots[i].target != pid)
      continue;
    __atomic_add_fetch(&g_slots[i].count, delta, __ATOMIC_RELAXED);
  }
}

void perf_pmu_tick(usize running_pid) { pmu_accumulate(running_pid); }

void perf_pmu_switch(usize prev_pid) { pmu_accumulate(prev_pid); }

#else /* !__x86_64__ */

/* aarch64 has PMUv3, which is a different register file (PMEVTYPER/PMEVCNTR
 * through the system-register space) and a different event numbering. Nothing
 * is pretended here: the counters are reported absent and perf_event_open goes
 * on refusing hardware events on this architecture. */
void perf_pmu_init(void) {}
int perf_pmu_available(void) { return 0; }
int perf_pmu_map(const struct perf_event_attr *a, u64 *evsel_out) {
  (void)a;
  (void)evsel_out;
  return -EOPNOTSUPP;
}
int perf_pmu_slot_alloc(u64 evsel, usize target, int user, int kernel) {
  (void)evsel;
  (void)target;
  (void)user;
  (void)kernel;
  return -EOPNOTSUPP;
}
void perf_pmu_slot_free(int slot) { (void)slot; }
u64 perf_pmu_slot_count(int slot) {
  (void)slot;
  return 0;
}
void perf_pmu_tick(usize running_pid) { (void)running_pid; }
void perf_pmu_switch(usize prev_pid) { (void)prev_pid; }

#endif
