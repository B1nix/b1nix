/*
 * PSI — pressure stall information.
 *
 * See b1nix/psi.h for what the numbers mean and where the stall regions are.
 * This file is only the bookkeeping: a counter of how many tasks are inside a
 * stall region right now, a per-tick accumulation of the time that counter was
 * non-zero, and the two-second decay that turns it into the three averages
 * every monitoring tool reads.
 *
 * The arithmetic is deliberately integer-only — the kernel has no FPU state of
 * its own (see the build rules) — so the averages are kept in hundredths of a
 * percent and printed by splitting off the last two digits.
 */

#include <b1nix/console.h>
#include <b1nix/ktime.h>
#include <b1nix/lapic.h>
#include <b1nix/psi.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <stdio.h>
#include <string.h>

/* Fixed point for the decay, the same shape Linux's loadavg and psi use:
 * FIXED_1 is 1.0, and each exponent is exp(-2/window) in those units. */
#define PSI_FSHIFT 11
#define PSI_FIXED_1 (1u << PSI_FSHIFT) /* 2048 */
static const u32 psi_exp[3] = {
    1126, /* exp(-2/10)  */
    1981, /* exp(-2/60)  */
    2034, /* exp(-2/300) */
};

/* How often the averages are recomputed, and the units `total` is printed in. */
#define PSI_WINDOW_NS (2000000000ull)
#define PSI_PCT_SCALE 10000u /* hundredths of a percent: 100.00% == 10000 */

struct psi_group {
  /* How many tasks are inside a stall region for this resource. Touched from
   * ISR-adjacent paths (the block layer completes under an interrupt), so it
   * moves only with atomics. */
  volatile int nr_stalled;
  u64 some_ns;
  u64 full_ns;
  /* What the last window started from, so a window measures a difference. */
  u64 some_ns_at_window;
  u64 full_ns_at_window;
  u32 some_avg[3];
  u32 full_avg[3];
};

static struct psi_group psi_groups[PSI_NR_RES];
static u64 psi_window_start_ns;
/* Runnable-but-not-running tasks as the scheduler last counted them. */
static volatile u32 psi_cpu_waiting;

void psi_stall_begin(enum psi_res res) {
  if ((unsigned)res >= PSI_NR_RES)
    return;
  __atomic_fetch_add(&psi_groups[res].nr_stalled, 1, __ATOMIC_RELAXED);
}

void psi_stall_end(enum psi_res res) {
  if ((unsigned)res >= PSI_NR_RES)
    return;
  /* Never below zero: an unpaired end would otherwise make the resource look
   * permanently unstalled, which is a silent loss of every later measurement. */
  int cur = __atomic_load_n(&psi_groups[res].nr_stalled, __ATOMIC_RELAXED);
  while (cur > 0) {
    if (__atomic_compare_exchange_n(&psi_groups[res].nr_stalled, &cur, cur - 1,
                                    1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      return;
  }
}

void psi_report_cpu_waiters(u32 waiting) {
  __atomic_store_n(&psi_cpu_waiting, waiting, __ATOMIC_RELAXED);
}

/* Is any CPU running something that is not its idle task? Used for `full`:
 * pressure is only "full" while nothing else is getting done. */
static int psi_any_cpu_productive(void) {
  for (int c = 0; c < g_max_cpus; c++) {
    struct percpu *pc = get_percpu_n(c);

    if (!pc || !pc->cpu_online)
      continue;
    struct task *ct = (struct task *)pc->cur_task;
    if (!ct)
      continue;
    if ((void *)ct == pc->idle_task)
      continue;
    if (ct->state != TASK_RUNNING)
      continue;
    return 1;
  }
  return 0;
}

void psi_tick(void) {
  u64 now = ktime_monotonic_ns();
  u32 hz = sched_tick_hz();
  u64 period_ns = hz ? 1000000000ull / hz : 0;

  if (!period_ns)
    return;

  int productive = psi_any_cpu_productive();

  for (int r = 0; r < PSI_NR_RES; r++) {
    struct psi_group *g = &psi_groups[r];
    int stalled;

    if (r == PSI_CPU) {
      /* A task waiting for a CPU cannot bracket its own wait, so the scheduler
       * hands over the count of runnable-but-not-running tasks instead. */
      stalled = (int)__atomic_load_n(&psi_cpu_waiting, __ATOMIC_RELAXED);
    } else {
      stalled = __atomic_load_n(&g->nr_stalled, __ATOMIC_RELAXED);
    }
    if (stalled <= 0)
      continue;
    g->some_ns += period_ns;
    if (!productive)
      g->full_ns += period_ns;
  }

  if (psi_window_start_ns == 0) {
    psi_window_start_ns = now;
    return;
  }
  u64 elapsed = now - psi_window_start_ns;
  if (elapsed < PSI_WINDOW_NS)
    return;
  psi_window_start_ns = now;

  for (int r = 0; r < PSI_NR_RES; r++) {
    struct psi_group *g = &psi_groups[r];
    u64 dsome = g->some_ns - g->some_ns_at_window;
    u64 dfull = g->full_ns - g->full_ns_at_window;

    g->some_ns_at_window = g->some_ns;
    g->full_ns_at_window = g->full_ns;

    u32 some_pct = (u32)((dsome * PSI_PCT_SCALE) / elapsed);
    u32 full_pct = (u32)((dfull * PSI_PCT_SCALE) / elapsed);

    if (some_pct > PSI_PCT_SCALE)
      some_pct = PSI_PCT_SCALE;
    if (full_pct > PSI_PCT_SCALE)
      full_pct = PSI_PCT_SCALE;

    for (int i = 0; i < 3; i++) {
      u32 e = psi_exp[i];

      g->some_avg[i] = (u32)(((u64)g->some_avg[i] * e +
                              (u64)some_pct * (PSI_FIXED_1 - e)) >>
                             PSI_FSHIFT);
      g->full_avg[i] = (u32)(((u64)g->full_avg[i] * e +
                              (u64)full_pct * (PSI_FIXED_1 - e)) >>
                             PSI_FSHIFT);
    }
  }
}

static usize psi_line(char *buf, usize cap, usize len, const char *what,
                      const u32 avg[3], u64 total_ns) {
  if (len >= cap)
    return len;
  int n = snprintf(buf + len, cap - len,
                   "%s avg10=%u.%02u avg60=%u.%02u avg300=%u.%02u total=%llu\n",
                   what, avg[0] / 100u, avg[0] % 100u, avg[1] / 100u,
                   avg[1] % 100u, avg[2] / 100u, avg[2] % 100u,
                   (unsigned long long)(total_ns / 1000ull));
  if (n < 0)
    return len;
  len += (usize)n;
  return len > cap ? cap : len;
}

usize psi_render(enum psi_res res, char *buf, usize cap) {
  if ((unsigned)res >= PSI_NR_RES || !buf || cap == 0)
    return 0;
  struct psi_group *g = &psi_groups[res];
  usize len = psi_line(buf, cap, 0, "some", g->some_avg, g->some_ns);

  /* Linux prints no `full` line for CPU pressure: a CPU stall by definition
   * leaves another task running, so the figure would always be zero. */
  if (res != PSI_CPU)
    len = psi_line(buf, cap, len, "full", g->full_avg, g->full_ns);
  return len;
}
