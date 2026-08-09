/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SCHED_H
#define LKPI_LINUX_SCHED_H
#include <linux/errno.h>
#include <lkpi/env.h>
#include <linux/types.h>
/* Scheduling, onto b1nix's. Ticks here are b1nix ticks (10 ms); imported code
 * that reasons in jiffies gets the same unit, which is what <linux/jiffies.h>
 * documents. */
#define TASK_RUNNING         0
#define TASK_INTERRUPTIBLE   1
#define TASK_UNINTERRUPTIBLE 2
#define MAX_SCHEDULE_TIMEOUT ((long)(~0UL >> 1))
static inline void cond_resched(void) { lkpi_yield(); }
static inline void set_current_state(int state) { (void)state; }
static inline void __set_current_state(int state) { (void)state; }
static inline long schedule_timeout(long timeout)
{
	if (timeout > 0)
		lkpi_sleep_ticks((u64)timeout);
	return 0;
}
static inline void schedule(void) { lkpi_yield(); }
static inline int signal_pending(void *t) { (void)t; return 0; }

/* `current` and struct task_struct come from <linux/types.h>, which every
 * imported file reaches; see there. */

/* Raise the submission thread to a real-time policy. b1nix has priorities but
 * not this interface; the request is declined and the thread runs at its
 * normal priority, which costs latency and not correctness. */
/* Wake a task that parked itself. lkpi's wait queues wake by channel, so this
 * is only reached for a task holding a pointer rather than a channel — nothing
 * in the core does, and the day one does it needs a real per-task wake. */
static inline int wake_up_process(void *task) { (void)task; return 0; }

static inline int sched_set_fifo(void *task) { (void)task; return -EINVAL; }
static inline int sched_set_fifo_low(void *task) { (void)task; return -EINVAL; }
#endif
