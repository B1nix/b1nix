/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_RCUWAIT_H
#define LKPI_LINUX_RCUWAIT_H

#include <linux/sched.h>
#include <linux/rcupdate.h>

/* One task waiting for a condition, woken by whoever makes it true. The task
 * pointer is published under RCU in Linux; here it is an atomic pointer the
 * waker reads, and the wake is lkpi's wake_up_process. */
struct rcuwait {
	struct task_struct *task;
};

#define __RCUWAIT_INITIALIZER(name) { .task = NULL, }

static inline void rcuwait_init(struct rcuwait *w)
{
	w->task = NULL;
}

static inline int rcuwait_active(struct rcuwait *w)
{
	return !!READ_ONCE(w->task);
}

extern int rcuwait_wake_up(struct rcuwait *w);

static inline void prepare_to_rcuwait(struct rcuwait *w)
{
	WRITE_ONCE(w->task, current);
}

static inline void finish_rcuwait(struct rcuwait *w)
{
	WRITE_ONCE(w->task, NULL);
	__set_current_state(TASK_RUNNING);
}

#define rcuwait_wait_event(w, condition, state)				\
({									\
	int __ret = 0;							\
	prepare_to_rcuwait(w);						\
	for (;;) {							\
		set_current_state(state);				\
		if (condition)						\
			break;						\
		if (signal_pending_state(state, current)) {		\
			__ret = -EINTR;					\
			break;						\
		}							\
		schedule();						\
	}								\
	finish_rcuwait(w);						\
	__ret;								\
})

#endif
