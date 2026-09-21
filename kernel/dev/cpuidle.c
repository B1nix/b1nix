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
 * not counted in ticks — a tick is exactly what does not happen here. */

#include <b1nix/cpuidle.h>

#include <b1nix/console.h>
#include <b1nix/ktime.h>
#include <b1nix/lapic.h>
#include <b1nix/types.h>

#include <string.h>

#define CPUIDLE_MAX_STATES 4

struct cpuidle_state {
  char name[8];
  char desc[32];
  u32 latency_us; /* the worst-case exit latency the CPU reports */
  u32 mwait_hint; /* the EAX MWAIT takes; only meaningful when use_mwait */
  u8 use_mwait;
};

static struct cpuidle_state g_states[CPUIDLE_MAX_STATES];
static int g_nstates;
static u64 g_usage[MAX_CPUS][CPUIDLE_MAX_STATES];
static u64 g_time_us[MAX_CPUS][CPUIDLE_MAX_STATES];

#if defined(__x86_64__)
static void cpuid_count(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
  __asm__ volatile("cpuid"
                   : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                   : "a"(leaf), "c"(sub));
}
#endif

static void state_add(const char *name, const char *desc, u32 latency_us,
                      int use_mwait, u32 hint) {
  struct cpuidle_state *s;

  if (g_nstates >= CPUIDLE_MAX_STATES)
    return;
  s = &g_states[g_nstates++];
  memset(s, 0, sizeof(*s));
  for (int i = 0; i < (int)sizeof(s->name) - 1 && name[i]; i++)
    s->name[i] = name[i];
  for (int i = 0; i < (int)sizeof(s->desc) - 1 && desc[i]; i++)
    s->desc[i] = desc[i];
  s->latency_us = latency_us;
  s->use_mwait = (u8)(use_mwait ? 1 : 0);
  s->mwait_hint = hint;
}

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
      if ((ec & 3u) == 3u) {
        /* EDX holds four bits of sub-state count per C-state. A C-state with
         * no sub-states is one this processor does not have. */
        for (int cstate = 0; cstate < 4; cstate++) {
          u32 substates = (ed >> (cstate * 4)) & 0xf;
          char name[8] = {'C', (char)('0' + cstate), 0, 0, 0, 0, 0, 0};
          char desc[32] = "MWAIT C0";

          if (!substates)
            continue;
          desc[7] = (char)('0' + cstate);
          /* The MWAIT hint is the C-state number in bits 4..7 and the
           * sub-state in bits 0..3; sub-state 0 is the shallowest of that
           * C-state, which is the one with a wake latency worth claiming. */
          state_add(name, desc, (u32)(cstate ? cstate * 10 : 1), 1,
                    (u32)(cstate << 4));
        }
      }
    }
  }
#endif
  if (!g_nstates) {
    /* Nothing to choose from: the halt instruction, named for what it is. */
#if defined(__x86_64__)
    state_add("C1", "HLT", 1, 0, 0);
#else
    state_add("C1", "WFI", 1, 0, 0);
#endif
  }
  console_write("cpuidle: ");
  console_write_dec((u64)g_nstates);
  console_write(" state(s),");
  for (int i = 0; i < g_nstates; i++) {
    console_write(" ");
    console_write(g_states[i].name);
    console_write("=");
    console_write(g_states[i].desc);
  }
  console_write("\n");
}

void cpuidle_enter(void) {
  struct percpu *pc = get_percpu();
  int cpu = pc ? (int)pc->cpu_id : 0;
  int state = g_nstates ? g_nstates - 1 : 0; /* the deepest this CPU has */
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

  start = ktime_monotonic_ns();

#if defined(__x86_64__)
  if (g_states[state].use_mwait) {
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
  } else {
    __asm__ volatile("sti; hlt" : : : "memory");
  }
#elif defined(__aarch64__)
  __asm__ volatile("msr daifclr, #2; wfi" : : : "memory");
#endif

  end = ktime_monotonic_ns();
  g_usage[cpu][state]++;
  if (end > start)
    g_time_us[cpu][state] += (end - start) / 1000;
}

int cpuidle_state_count(void) { return g_nstates; }

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

u64 cpuidle_state_usage(int cpu, int state) {
  if (cpu < 0 || cpu >= MAX_CPUS || state < 0 || state >= g_nstates)
    return 0;
  return g_usage[cpu][state];
}

u64 cpuidle_state_time_us(int cpu, int state) {
  if (cpu < 0 || cpu >= MAX_CPUS || state < 0 || state >= g_nstates)
    return 0;
  return g_time_us[cpu][state];
}
