/* SPDX-License-Identifier: GPL-2.0-only */
/* perf_event_open(2) — software counters, sampling and the mmap'd ring buffer
 * `perf` reads (M126). See kernel/perf/perf_event.c. */
#ifndef B1NIX_PERF_EVENT_H
#define B1NIX_PERF_EVENT_H

#include <b1nix/types.h>

struct task;

/* The system-call hook. Returns 1 and sets *ret when `nr` is perf_event_open's
 * number on this architecture, 0 otherwise. */
int perf_event_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                       u64 *ret);

/* Called from the per-CPU timer tick, on the CPU that took it, with the
 * register file of whatever it interrupted. This is where a sampling event's
 * period is charged and where a PERF_RECORD_SAMPLE is written. */
void perf_event_tick_sample(u64 pc, u64 fp, int in_user, int cpu);

/* Called when a task is about to leave: flushes PERF_RECORD_EXIT so a `perf
 * report` can close the map of a process that has gone. */
void perf_event_task_exit(struct task *t);

/* The value /proc/sys/kernel/perf_event_paranoid reports, and the setting that
 * decides whether an unprivileged process may open an event. */
int perf_event_paranoid_get(void);
void perf_event_paranoid_set(int v);

#endif
