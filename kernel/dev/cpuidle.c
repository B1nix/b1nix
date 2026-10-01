/* SPDX-License-Identifier: GPL-2.0-only */
/* Idle states (M129).
 *
 * Parking a CPU used to be `sti; hlt` written out at each of the four places
 * that do it, with no record that it had happened. That is enough to stop
 * burning a core, and not enough for anything else: a machine cannot say how
 * much of its time it spends asleep, a deeper C-state is never entered on a
 * processor that has one, and powertop finds no /sys/devices/system/cpu to
 * read. All four sites now come here.
 *
 * What the instruction is depends on the processor. MONITOR/MWAIT (CPUID
 * leaf 1, ECX bit 3) with the extensions of leaf 5 lets a core be told which
 * C-state to enter and to wake on an interrupt even with interrupts masked —
 * which closes the window between "decided to idle" and "halted" without the
 * sti/hlt pairing trick. A processor without it, and every QEMU guest that is
 * not started with `-overcommit cpu-pm=on`, gets HLT, and the state list says
 * so: one state, named C1, described as "HLT".
 *
 * The counters are what the tickless work is measured with, so they are per
 * CPU and they are real: entry and exit are stamped with the monotonic clock,
 * not counted in ticks — a tick is exactly what does not happen here.
 *
 * M135 adds the two halves Linux has and this did not. The states a platform
 * declares in ACPI `_CST` — the MWAIT hint for each, or the I/O port whose
 * read enters it, and the firmware's own exit latency — replace the guessed
 * list where the firmware has one. And a governor chooses among them: the
 * deepest state was taken on every idle, so a CPU woken every 50 us paid a
 * deep state's exit each time. The choice is Linux's menu governor in outline
 * — the idle is predicted from the next timer and from the recent idles, and
 * the deepest state whose residency fits the prediction wins — and `above`
 * and `below` count the choices that turned out too deep or too shallow. */

#include <b1nix/cpuidle.h>

#include <b1nix/aml.h>
#include <b1nix/console.h>
#if defined(__x86_64__)
#include <b1nix/io.h>
#endif
#include <b1nix/ktime.h>
#include <b1nix/lapic.h>
#include <b1nix/sched.h>
#include <b1nix/types.h>

#include <stdio.h>
#include <string.h>

#define CPUIDLE_MAX_STATES 8

enum { ENTER_HALT, ENTER_MWAIT, ENTER_IO };

struct cpuidle_state {
  char name[8];
  char desc[32];
  u32 latency_us;   /* the worst-case exit latency */
  u32 residency_us; /* the shortest idle that repays entering it */
  u32 power_mw;     /* what the firmware says it draws; 0 when unknown */
  u32 mwait_hint;   /* ENTER_MWAIT: the EAX MWAIT takes */
  u16 io_port;      /* ENTER_IO: the port whose read enters it */
  u8 method;
};

static struct cpuidle_state g_states[CPUIDLE_MAX_STATES];
static int g_nstates;
static const char *g_driver = "none";
static u64 g_usage[MAX_CPUS][CPUIDLE_MAX_STATES];
static u64 g_time_us[MAX_CPUS][CPUIDLE_MAX_STATES];
static u64 g_above[MAX_CPUS][CPUIDLE_MAX_STATES];
static u64 g_below[MAX_CPUS][CPUIDLE_MAX_STATES];
static u8 g_disabled[MAX_CPUS][CPUIDLE_MAX_STATES];

/* The menu governor's memory: the last few idles each CPU really had, and
 * how far the timer overstated the idle, per order of magnitude of the timer
 * (Linux's correction factors: a running average of measured / next timer,
 * scaled by RESOLUTION * DECAY, where RESOLUTION * DECAY means "exactly"). */
#define IDLE_HISTORY 8
#define MENU_BUCKETS 6
#define MENU_RESOLUTION 1024u
#define MENU_DECAY 8u
static u32 g_hist_us[MAX_CPUS][IDLE_HISTORY];
static u8 g_hist_pos[MAX_CPUS];
static u32 g_correction[MAX_CPUS][MENU_BUCKETS];
static u64 g_next_us[MAX_CPUS];
static u8 g_bucket[MAX_CPUS];

#if defined(__x86_64__)
static int g_mwait_ok; /* MWAIT that wakes on a masked interrupt */

static void cpuid_count(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(sub));
}
#endif

static void copy_str(char *dst, usize cap, const char *src) {
  usize i = 0;

  for (; i + 1 < cap && src[i]; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

static void state_add(const char *name, const char *desc, u32 latency_us,
                      u32 power_mw, int method, u32 hint, u16 port) {
  struct cpuidle_state *s;

  if (g_nstates >= CPUIDLE_MAX_STATES)
    return;
  s = &g_states[g_nstates++];
  memset(s, 0, sizeof(*s));
  copy_str(s->name, sizeof(s->name), name);
  copy_str(s->desc, sizeof(s->desc), desc);
  s->latency_us = latency_us;
  /* Linux's acpi_idle asks for twice the exit latency before a state pays
   * for itself; the same factor serves the probed states. */
  s->residency_us = latency_us * 2;
  s->power_mw = power_mw;
  s->method = (u8)method;
  s->mwait_hint = hint;
  s->io_port = port;
}

/* ── ACPI _CST ──────────────────────────────────────────────────────────── */

#if defined(__x86_64__)
#define CST_PATH_MAX 96
#define CST_SPACE_IO 0x01u
#define CST_SPACE_FFH 0x7fu
/* FFH register, as Intel defines it for _CST: bit_width is the vendor (1 =
 * Intel), bit_offset the class (1 = C1 by HLT, 2 = MWAIT), address the hint. */
#define CST_FFH_CLASS_MWAIT 2u

struct cst_scan {
  char (*node)[CST_PATH_MAX];
  int n, max;
};

/* The walk runs with the interpreter's lock held: collect, evaluate later. */
static void cst_scan_one(void *ctx, const char *path, int type) {
  struct cst_scan *sc = (struct cst_scan *)ctx;

  if (strlen(path) >= CST_PATH_MAX || sc->n >= sc->max)
    return;
  if (type == AML_T_PROCESSOR || type == AML_T_DEVICE)
    copy_str(sc->node[sc->n++], CST_PATH_MAX, path);
}

/* One state of a _CST: {Register, Type, Latency, Power}. */
static int cst_state(const char *path, u32 idx) {
  struct aml_result reg, st;
  const u8 *b;
  u8 space, width, offset;
  u64 addr = 0;
  u32 type, lat, power;
  char name[8], desc[32];

  if (aml_evaluate_element(path, 0, 0, idx, &st) != AML_OK ||
      st.type != AML_T_PACKAGE || st.elems < 4 ||
      aml_evaluate_subelement(path, 0, 0, idx, 0, &reg) != AML_OK ||
      reg.type != AML_T_BUFFER || reg.bytes_copied < 15 || reg.bytes[0] != 0x82)
    return 0;
  b = reg.bytes;
  space = b[3];
  width = b[4];
  offset = b[5];
  for (int i = 0; i < 8; i++)
    addr |= (u64)b[7 + i] << (i * 8);
  type = (u32)st.elem_int[1];
  lat = (u32)st.elem_int[2];
  power = (u32)st.elem_int[3];
  if (type < 1 || type > 3)
    return 0;
  snprintf(name, sizeof(name), "C%u", (unsigned)type);
  if (space == CST_SPACE_FFH && width == 1 && offset == CST_FFH_CLASS_MWAIT) {
    if (!g_mwait_ok)
      return 0; /* a hint for an instruction this processor will not take */
    snprintf(desc, sizeof(desc), "ACPI FFH MWAIT 0x%x", (unsigned)addr);
    state_add(name, desc, lat, power, ENTER_MWAIT, (u32)addr, 0);
    return 1;
  }
  if (type == 1) {
    /* C1 is the halt instruction whatever register it names. */
    state_add(name, "ACPI HLT", lat, power, ENTER_HALT, 0, 0);
    return 1;
  }
  if (space == CST_SPACE_IO && addr && addr <= 0xffff) {
    snprintf(desc, sizeof(desc), "ACPI IOPORT 0x%x", (unsigned)addr);
    state_add(name, desc, lat, power, ENTER_IO, 0, (u16)addr);
    return 1;
  }
  return 0; /* a register this kernel cannot enter the state through */
}

/* The C-states the firmware declares for a processor, or 0 when it declares
 * none. All CPUs get the first processor's list, as every _CST in practice is
 * the same table repeated per processor. */
static int cst_probe(void) {
  static char nodes[64][CST_PATH_MAX];
  struct cst_scan sc = {nodes, 0, 64};

  if (!aml_ready() || aml_table_count() == 0)
    return 0;
  aml_walk(cst_scan_one, &sc);
  for (int i = 0; i < sc.n; i++) {
    char path[CST_PATH_MAX + 8];
    struct aml_result r;
    u32 count;

    snprintf(path, sizeof(path), "%s._CST", nodes[i]);
    if (!aml_exists(path) || aml_evaluate(path, 0, 0, &r) != AML_OK ||
        r.type != AML_T_PACKAGE || r.length < 2 ||
        aml_evaluate_element(path, 0, 0, 0, &r) != AML_OK ||
        r.type != AML_T_INTEGER)
      continue;
    count = (u32)r.integer;
    g_nstates = 0;
    for (u32 k = 1; k <= count && g_nstates < CPUIDLE_MAX_STATES; k++)
      (void)cst_state(path, k);
    if (g_nstates)
      return 1;
  }
  return 0;
}
#endif

void cpuidle_init(void) {
  if (g_nstates)
    return;
#if defined(__x86_64__)
  {
    u32 a, b, c, d;

    cpuid_count(1, 0, &a, &b, &c, &d);
    if (c & (1u << 3)) { /* MONITOR/MWAIT */
      u32 ea, eb, ec, ed;

      cpuid_count(5, 0, &ea, &eb, &ec, &ed);
      /* ECX bit 0: the leaf's extensions are valid at all; bit 1: MWAIT can
       * be told to wake on an interrupt with interrupts masked, which is the
       * property that makes it usable here. Without it, MWAIT would have to
       * be paired with sti the way HLT is, and then it buys nothing. */
      g_mwait_ok = (ec & 3u) == 3u;
    }
  }
  /* What the firmware declares comes first: its latencies are measured on
   * this platform, the probe's are guesses. */
  if (cst_probe()) {
    g_driver = "acpi_idle";
  } else if (g_mwait_ok) {
    u32 a, b, c, d;

    cpuid_count(5, 0, &a, &b, &c, &d);
    /* EDX holds four bits of sub-state count per C-state. A C-state with no
     * sub-states is one this processor does not have. */
    for (int cstate = 0; cstate < 4; cstate++) {
      u32 substates = (d >> (cstate * 4)) & 0xf;
      char name[8], desc[32];

      if (!substates)
        continue;
      snprintf(name, sizeof(name), "C%d", cstate);
      snprintf(desc, sizeof(desc), "MWAIT C%d", cstate);
      /* The MWAIT hint is the C-state number in bits 4..7 and the sub-state
       * in bits 0..3; sub-state 0 is the shallowest of that C-state, which is
       * the one with a wake latency worth claiming. */
      state_add(name, desc, (u32)(cstate ? cstate * 10 : 1), 0, ENTER_MWAIT,
                (u32)(cstate << 4), 0);
    }
    if (g_nstates)
      g_driver = "mwait_idle";
  }
#endif
  if (!g_nstates) {
    /* Nothing to choose from: the halt instruction, named for what it is. */
#if defined(__x86_64__)
    state_add("C1", "HLT", 1, 0, ENTER_HALT, 0, 0);
#else
    state_add("C1", "WFI", 1, 0, ENTER_HALT, 0, 0);
#endif
    g_driver = "halt_idle";
  }
  console_write("cpuidle: ");
  console_write(g_driver);
  console_write(", ");
  console_write_dec((u64)g_nstates);
  console_write(" state(s),");
  for (int i = 0; i < g_nstates; i++) {
    console_write(" ");
    console_write(g_states[i].name);
    console_write("=");
    console_write(g_states[i].desc);
    console_write("/");
    console_write_dec(g_states[i].latency_us);
    console_write("us");
  }
  console_write("\n");
}

/* ── the governor ───────────────────────────────────────────────────────── */

/* Microseconds until this CPU's timer fires: the deadline a tickless idle
 * programmed, or one tick when the timer is periodic. */
static u64 next_timer_us(struct percpu *pc) {
  u32 hz = sched_tick_hz() ? sched_tick_hz() : 100;
  u64 tick_us = 1000000ull / hz;
  u64 deadline = pc ? __atomic_load_n(&pc->timer_deadline_tick,
                                      __ATOMIC_ACQUIRE)
                    : 0;
  u64 now = scheduler_get_ticks();

  if (deadline > now)
    return (deadline - now) * tick_us;
  return tick_us;
}

/* Linux's get_typical_interval: the mean of the recent idles when they agree
 * with each other, dropping the longest as an outlier while that makes them
 * agree; ~0 when they do not. */
static u64 typical_us(int cpu) {
  u32 thresh = ~0u;

  for (int round = 0; round < IDLE_HISTORY / 4; round++) {
    u64 sum = 0, sq = 0, max = 0;
    int n = 0;

    for (int i = 0; i < IDLE_HISTORY; i++) {
      u64 v = g_hist_us[cpu][i];

      if (!v || v > thresh)
        continue;
      sum += v;
      sq += v * v;
      if (v > max)
        max = v;
      n++;
    }
    if (n < IDLE_HISTORY * 3 / 4)
      return ~0ull;
    {
      u64 avg = sum / (u64)n;
      u64 mean_sq = sq / (u64)n;
      u64 var = mean_sq > avg * avg ? mean_sq - avg * avg : 0;

      /* Tight enough: within a sixth either way, or 20 us of deviation. */
      if (var <= 400 || avg * avg > 36 * var)
        return avg;
    }
    thresh = (u32)max - 1;
  }
  return ~0ull;
}

static int menu_bucket(u64 us) {
  int b = 0;

  for (u64 lim = 10; b < MENU_BUCKETS - 1 && us >= lim; lim *= 10)
    b++;
  return b;
}

static int select_state(int cpu, struct percpu *pc) {
  u64 next = next_timer_us(pc);
  int b = menu_bucket(next);
  u32 cf = g_correction[cpu][b] ? g_correction[cpu][b]
                                : MENU_RESOLUTION * MENU_DECAY;
  u64 predict = next * cf / (MENU_RESOLUTION * MENU_DECAY);
  u64 typ = typical_us(cpu);
  int pick = -1;

  g_next_us[cpu] = next;
  g_bucket[cpu] = (u8)b;

  if (typ < predict)
    predict = typ;
  for (int i = 0; i < g_nstates; i++) {
    if (g_disabled[cpu][i])
      continue;
    if (pick < 0) {
      pick = i; /* the shallowest allowed, whatever the prediction */
      continue;
    }
    if (g_states[i].residency_us > predict)
      break;
    pick = i;
  }
  return pick;
}

static void account(int cpu, int state, u64 slept_us) {
  g_usage[cpu][state]++;
  g_time_us[cpu][state] += slept_us;
  if (slept_us < g_states[state].residency_us)
    g_above[cpu][state]++;
  else
    for (int j = state + 1; j < g_nstates; j++)
      if (!g_disabled[cpu][j] && slept_us >= g_states[j].residency_us) {
        g_below[cpu][state]++;
        break;
      }
  {
    u32 cf = g_correction[cpu][g_bucket[cpu]];
    u64 next = g_next_us[cpu];

    if (!cf)
      cf = MENU_RESOLUTION * MENU_DECAY;
    cf -= cf / MENU_DECAY;
    cf += next && slept_us < next
              ? (u32)(MENU_RESOLUTION * slept_us / next)
              : MENU_RESOLUTION;
    g_correction[cpu][g_bucket[cpu]] = cf ? cf : 1;
  }
  g_hist_us[cpu][g_hist_pos[cpu]] = slept_us > 0xffffffffull
                                        ? 0xffffffffu
                                        : (u32)(slept_us ? slept_us : 1);
  g_hist_pos[cpu] = (u8)((g_hist_pos[cpu] + 1) % IDLE_HISTORY);
}

void cpuidle_enter(void) {
  struct percpu *pc = get_percpu();
  int cpu = pc ? (int)pc->cpu_id : 0;
  int state;
  u64 start, end;

  if (!g_nstates) {
    /* Before the probe ran: halt, and do not pretend to have measured it. */
#if defined(__x86_64__)
    __asm__ volatile("sti; hlt" : : : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("msr daifclr, #2; wfi" : : : "memory");
#endif
    return;
  }
  if (cpu < 0 || cpu >= MAX_CPUS)
    cpu = 0;
  state = select_state(cpu, pc);
  if (state < 0)
    state = 0; /* every state disabled: Linux polls, this halts */

  start = ktime_monotonic_ns();

#if defined(__x86_64__)
  switch (g_states[state].method) {
  case ENTER_MWAIT: {
    /* MONITOR an address of our own, then MWAIT with ECX bit 0 set: break out
     * on an interrupt even though they are masked. Nothing else writes the
     * monitored line, so the wake is the interrupt. */
    volatile u64 monitor_line;

    __asm__ volatile("monitor" : : "a"(&monitor_line), "c"(0), "d"(0) : "memory");
    __asm__ volatile("mwait" : : "a"(g_states[state].mwait_hint), "c"(1)
                     : "memory");
    /* MWAIT returned with interrupts still masked; let the handler run, which
     * is what the caller of an idle routine expects to have happened. */
    __asm__ volatile("sti" : : : "memory");
    break;
  }
  case ENTER_IO:
    /* The read is the request: the chipset stops the clock until a break
     * event, which an interrupt is even with interrupts masked. The halt that
     * follows returns at once on real hardware — the interrupt that ended the
     * state is pending — and is what keeps a platform that ignores the read
     * (a guest) from spinning. */
    (void)inb(g_states[state].io_port);
    __asm__ volatile("sti; hlt" : : : "memory");
    break;
  default:
    __asm__ volatile("sti; hlt" : : : "memory");
    break;
  }
#elif defined(__aarch64__)
  __asm__ volatile("msr daifclr, #2; wfi" : : : "memory");
#endif

  end = ktime_monotonic_ns();
  account(cpu, state, end > start ? (end - start) / 1000 : 0);
}

int cpuidle_state_count(void) { return g_nstates; }
const char *cpuidle_driver_name(void) { return g_driver; }
const char *cpuidle_governor_name(void) { return "menu"; }

const char *cpuidle_state_name(int state) {
  if (state < 0 || state >= g_nstates)
    return "";
  return g_states[state].name;
}

const char *cpuidle_state_desc(int state) {
  if (state < 0 || state >= g_nstates)
    return "";
  return g_states[state].desc;
}

u32 cpuidle_state_latency_us(int state) {
  if (state < 0 || state >= g_nstates)
    return 0;
  return g_states[state].latency_us;
}

u32 cpuidle_state_residency_us(int state) {
  if (state < 0 || state >= g_nstates)
    return 0;
  return g_states[state].residency_us;
}

u32 cpuidle_state_power_mw(int state) {
  if (state < 0 || state >= g_nstates)
    return 0;
  return g_states[state].power_mw;
}

#define CPU_STATE_OK(cpu, state)                                             \
  ((cpu) >= 0 && (cpu) < MAX_CPUS && (state) >= 0 && (state) < g_nstates)

u64 cpuidle_state_usage(int cpu, int state) {
  return CPU_STATE_OK(cpu, state) ? g_usage[cpu][state] : 0;
}

u64 cpuidle_state_time_us(int cpu, int state) {
  return CPU_STATE_OK(cpu, state) ? g_time_us[cpu][state] : 0;
}

u64 cpuidle_state_above(int cpu, int state) {
  return CPU_STATE_OK(cpu, state) ? g_above[cpu][state] : 0;
}

u64 cpuidle_state_below(int cpu, int state) {
  return CPU_STATE_OK(cpu, state) ? g_below[cpu][state] : 0;
}

int cpuidle_state_disabled(int cpu, int state) {
  return CPU_STATE_OK(cpu, state) ? g_disabled[cpu][state] : 0;
}

int cpuidle_state_set_disabled(int cpu, int state, int disabled) {
  if (!CPU_STATE_OK(cpu, state))
    return -1;
  __atomic_store_n(&g_disabled[cpu][state], (u8)(disabled ? 1 : 0),
                   __ATOMIC_RELEASE);
  return 0;
}
