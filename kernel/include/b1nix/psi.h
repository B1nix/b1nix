/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_PSI_H
#define B1NIX_PSI_H

#include <b1nix/types.h>

struct task;

/*
 * PSI — pressure stall information (kernel/mm/psi.c).
 *
 * /proc/pressure/{cpu,memory,io}, in the format Linux prints and the format
 * systemd-oomd, systemd's MemoryPressureWatch and every monitoring agent
 * parses:
 *
 *   some avg10=0.00 avg60=0.00 avg300=0.00 total=0
 *   full avg10=0.00 avg60=0.00 avg300=0.00 total=0
 *
 * `total` is microseconds of stall since boot; the averages are the share of
 * wall time stalled over the last 10, 60 and 300 seconds, decayed on the same
 * two-second window and with the same exponents Linux uses.
 *
 * What counts as a stall is decided at the call sites, not here: a task is
 * stalled on a resource for exactly as long as it is inside a
 * psi_stall_begin()/psi_stall_end() pair for it. The pairs live where the
 * waiting really happens -- the block layer's device gate (io), the page
 * reclaim and swap-in paths (memory) -- so a number here is a measurement and
 * not a model.
 *
 * `some` is charged while at least one task is inside such a region. `full` is
 * charged while at least one task is and no CPU is running anything else --
 * b1nix's reading of Linux's "every non-idle task is stalled". A CPU spinning
 * inside reclaim is running something, so a reclaim-bound machine reports a
 * high `some` and a low `full`; a machine whose every task is waiting for a
 * disk reports both. CPU pressure has no `full` line, exactly as on Linux.
 */

enum psi_res {
  PSI_IO = 0,
  PSI_MEM = 1,
  PSI_CPU = 2,
  PSI_NR_RES = 3,
};

/* Enter/leave a stall region. Must be paired on every path, including the
 * error ones; the counter is global and a leaked begin would charge pressure
 * for ever. Safe from any context, including with interrupts off. */
void psi_stall_begin(enum psi_res res);
void psi_stall_end(enum psi_res res);

/* Called once per timer tick from the BSP. Accumulates the stall totals and,
 * every two seconds, folds them into the decaying averages. */
void psi_tick(void);

/* CPU pressure has no call sites of its own: a task waiting for a CPU is not
 * executing, so it cannot bracket its own wait. The scheduler reports instead
 * how many tasks were runnable-but-not-running when the tick fired. */
void psi_report_cpu_waiters(u32 waiting);

/* Render one resource's two lines into `buf`; returns the length written. */
usize psi_render(enum psi_res res, char *buf, usize cap);

/* ── per-cgroup pressure (cpu.pressure, memory.pressure, io.pressure) ──────
 *
 * The same bookkeeping, kept per cgroup by kernel/fs/cgroup/cgroup.c: it
 * samples which tasks are stalled and which are getting work done, charges
 * each cgroup and its ancestors, and folds the totals into the same decaying
 * averages. A cgroup's `full` is its own: every non-idle member stalled at
 * once, which is what systemd-oomd watches a unit's memory.pressure for. */
#define PSI_WINDOW_NS (2000000000ull)

struct psi_group {
  /* How many tasks are inside a stall region for this resource. Used only by
   * the machine-wide groups; touched from ISR-adjacent paths (the block layer
   * completes under an interrupt), so it moves only with atomics. */
  volatile int nr_stalled;
  u64 some_ns;
  u64 full_ns;
  /* What the last window started from, so a window measures a difference. */
  u64 some_ns_at_window;
  u64 full_ns_at_window;
  u32 some_avg[3];
  u32 full_avg[3];
};

/* Charge one sample of `period_ns` to `g`: `some` when a task was stalled,
 * `full` when nothing was getting done besides. */
void psi_group_sample(struct psi_group *g, int some, int full, u64 period_ns);
/* Fold the window that just ended (`elapsed_ns` long) into the averages. */
void psi_group_fold(struct psi_group *g, u64 elapsed_ns);
/* Render a group's lines. `with_full` is 0 only for the machine-wide CPU. */
usize psi_group_render(const struct psi_group *g, int with_full, char *buf,
                       usize cap);

/* Is anything on the machine stalled right now -- a task in a memory or io
 * stall region, or one waiting for a CPU? When nothing is, no cgroup can be
 * either, and the per-cgroup sample has nothing to walk for. */
int psi_machine_stalled(void);

/* Is task `t` inside a stall region for `res` (memory or io) right now? */
int psi_task_stalled(const struct task *t, enum psi_res res);

#endif /* B1NIX_PSI_H */
