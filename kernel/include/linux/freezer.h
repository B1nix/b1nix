/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FREEZER_H
#define LKPI_LINUX_FREEZER_H

/*
 * The task freezer, which b1nix does not have: nothing here suspends to disk
 * and nothing asks a kernel thread to stop and be restarted where it was.
 *
 * So `freezing()` is always false and `try_to_freeze()` always says it did not
 * freeze. That is the honest answer rather than a convenient one — a caller
 * that believes it was frozen and re-checks its state would be doing extra work
 * on a false report, and every caller here is a loop that simply carries on.
 */

struct task_struct;

static inline int freezing(struct task_struct *p) { (void)p; return 0; }
static inline int try_to_freeze(void) { return 0; }
static inline void set_freezable(void) { }
static inline void __set_freezable(void) { }

#define freezable_schedule()          schedule()
#define freezable_schedule_timeout(t) schedule_timeout(t)
#define wait_event_freezable(wq, cond)         wait_event_interruptible(wq, cond)
#define wait_event_freezable_timeout(wq, cond, t) \
	wait_event_interruptible_timeout(wq, cond, t)

#endif
