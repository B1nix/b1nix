/* SPDX-License-Identifier: MIT */
#ifndef LKPI_WAIT_H
#define LKPI_WAIT_H

#include <lkpi/env.h>
#include <lkpi/types.h>

/*
 * wait queues — "sleep until this condition holds".
 *
 * A completion answers "has this one thing finished"; a wait queue answers "is
 * this predicate true yet", which is what a driver needs when the thing it is
 * waiting for is a ring index, a register bit or a queue depth rather than a
 * single event. The predicate is re-evaluated after every wakeup, so a spurious
 * or shared wakeup costs a re-check and nothing else.
 *
 * The lost-wakeup race is closed the same way the rest of this layer closes it:
 * the waiter publishes itself on the channel with scheduler_wait_prepare, then
 * re-tests the predicate, and only parks if it is still false. A wake that
 * lands between the test and the park therefore cannot be missed — it is
 * already visible to the re-test.
 *
 * wake_up() wakes every waiter on the queue rather than exactly one. That is
 * more wakeups than Linux would issue for a non-exclusive wait, but never fewer,
 * so no waiter whose predicate became true is left parked; each one re-tests and
 * the ones still false park again. Correctness does not depend on which waiter
 * runs first, and a driver that needs one-at-a-time hand-off wants a mutex.
 *
 * wait_event*() sleeps: never call it holding a spinlock. wake_up() does not
 * sleep and is safe from an interrupt handler.
 */

struct wait_queue_head {
	volatile u64 wakeups; /* wake_up calls; diagnostics and self-test */
	volatile u32 waiters; /* tasks currently parked or about to park */
};

#ifndef LKPI_WAIT_QUEUE_HEAD_T_DEFINED
#define LKPI_WAIT_QUEUE_HEAD_T_DEFINED
typedef struct wait_queue_head wait_queue_head_t;
#endif

void init_waitqueue_head(struct wait_queue_head *wq);

/* Wake every waiter. Does not sleep; safe from an interrupt handler. */
void wake_up(struct wait_queue_head *wq);
/* Spelled separately because drivers use both names; identical behaviour. */
void wake_up_all(struct wait_queue_head *wq);

/* Number of wake_up calls this queue has seen. */
u64 waitqueue_wakeups(const struct wait_queue_head *wq);
/* Tasks currently waiting. */
u32 waitqueue_waiters(const struct wait_queue_head *wq);

/*
 * Internal helpers used by the macros below. They exist as functions so the
 * macros stay small and so the non-sleepable-context path lives in one place.
 */
void lkpi_wait_enter(struct wait_queue_head *wq);
void lkpi_wait_leave(struct wait_queue_head *wq);
/* 1 when the caller may park, 0 when it must spin (early boot, IRQ context). */
int lkpi_wait_may_block(void);
/* One spin iteration for the cannot-park case: pause + TLB shootdown service. */
void lkpi_wait_relax(void);

/*
 * Sleep until `condition` evaluates true.
 *
 * `condition` is an expression re-evaluated on every pass, exactly as in the
 * Linux macro, so it must be cheap and free of side effects.
 */
#define wait_event(wq, condition)                                              \
	do {                                                                       \
		if (!(condition)) {                                                    \
			lkpi_wait_enter(&(wq));                                            \
			for (;;) {                                                         \
				if (condition)                                                 \
					break;                                                     \
				if (!lkpi_wait_may_block()) {                                  \
					lkpi_wait_relax();                                         \
					continue;                                                  \
				}                                                              \
				lkpi_wait_prepare(&(wq));                                 \
				if (condition) {                                               \
					lkpi_wait_cancel();                                   \
					break;                                                     \
				}                                                              \
				lkpi_wait_commit();                                        \
			}                                                                  \
			lkpi_wait_leave(&(wq));                                            \
		}                                                                      \
	} while (0)

/*
 * As above with a deadline in scheduler ticks (10 ms each). `ret` is set to the
 * ticks remaining when the condition became true (at least 1), or 0 on timeout —
 * the same "0 means it timed out" convention wait_for_completion_timeout uses.
 */
#define wait_event_timeout_r(wq, condition, timeout_ticks, ret)                  \
	do {                                                                       \
		u64 lkpi__start = lkpi_ticks();                               \
		u64 lkpi__limit = (timeout_ticks);                                     \
		(ret) = 1;                                                             \
		if (!(condition)) {                                                    \
			lkpi_wait_enter(&(wq));                                            \
			for (;;) {                                                         \
				if (condition)                                                 \
					break;                                                     \
				u64 lkpi__spent = lkpi_ticks() - lkpi__start;         \
				if (lkpi__spent >= lkpi__limit) {                              \
					/* Re-test once more before declaring a timeout: the       \
					 * condition may have become true while we were deciding. */ \
					(ret) = (condition) ? 1 : 0;                               \
					break;                                                     \
				}                                                              \
				if (!lkpi_wait_may_block()) {                                  \
					lkpi_wait_relax();                                         \
					continue;                                                  \
				}                                                              \
				lkpi_wait_prepare_timeout(&(wq),                          \
				                               lkpi__limit - lkpi__spent);     \
				if (condition) {                                               \
					lkpi_wait_cancel();                                   \
					break;                                                     \
				}                                                              \
				lkpi_wait_commit();                                        \
			}                                                                  \
			lkpi_wait_leave(&(wq));                                            \
			if ((ret) != 0) {                                                  \
				u64 lkpi__done = lkpi_ticks() - lkpi__start;           \
				(ret) = (lkpi__done >= lkpi__limit)                            \
				            ? 1                                                \
				            : (lkpi__limit - lkpi__done);                      \
			}                                                                  \
		}                                                                      \
	} while (0)

#endif
