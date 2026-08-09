/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_WAIT_H
#define LKPI_LINUX_WAIT_H
#include <lkpi/wait.h>
#include <linux/kernel.h>
/* Onto lkpi's wait queues: the predicate is re-tested after every wake, and the
 * two-phase park closes the lost-wakeup race. */
#define wake_up_interruptible(wq) wake_up(wq)
#define wake_up_all_interruptible(wq) wake_up_all(wq)
/*
 * A statement-expression, because wait_event is a statement and imported code
 * assigns the result. Always 0: b1nix kernel threads are not signal targets, so
 * the wait cannot be interrupted and a caller checking for -ERESTARTSYS never
 * sees it.
 */
#define wait_event_interruptible(wq, cond)  \
	({                                       \
		wait_event(wq, cond);                \
		0;                                   \
	})

/* Waking only the waiters interested in particular poll events. lkpi's queues
 * do not carry per-waiter event masks, so every waiter is woken and re-tests
 * its own predicate — more wakeups, never fewer. */
#define wake_up_interruptible_poll(wq, events) do { (void)(events); wake_up(wq); } while (0)
#define wake_up_poll(wq, events)              wake_up_interruptible_poll(wq, events)
#define wake_up_interruptible_all(wq)         wake_up_all(wq)

/*
 * Linux's form is an expression yielding the ticks remaining, so it is written
 * as a statement-expression here rather than through an out-parameter. The
 * out-parameter version lives in <lkpi/wait.h> as wait_event_timeout_r, which
 * this wraps.
 */
#define wait_event_timeout(wq, condition, timeout_ticks)     \
	({                                                       \
		u64 __ret = 0;                                       \
		wait_event_timeout_r(wq, condition, timeout_ticks, __ret); \
		__ret;                                               \
	})

#define wait_event_interruptible_timeout(wq, cond, t) \
	wait_event_timeout(wq, cond, t)

/*
 * Wait with a lock held: drop it around the sleep and retake it before
 * re-testing. Dropping is not optional — the condition is changed by someone
 * who needs that same lock, so holding it across the sleep is a deadlock.
 */
#define wait_event_lock_irq(wq, condition, lock)     \
	do {                                             \
		while (!(condition)) {                       \
			spin_unlock(&(lock));                    \
			wait_event(wq, condition);               \
			spin_lock(&(lock));                      \
		}                                            \
	} while (0)
#endif
