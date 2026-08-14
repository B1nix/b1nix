/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_WAIT_H
#define LKPI_WAIT_H

#include <lkpi/env.h>
#include <linux/list.h>
#include <lkpi/lock.h>
#include <lkpi/types.h>

/* <linux/list.h> and <lkpi/lock.h> rather than b1nix's own: this header is
 * included by imported translation units, which must never see a b1nix header.
 * Both are ours and GPL-2.0-only — see the boundary note in <lkpi/env.h>. */

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

/*
 * Per-waiter entries.
 *
 * The queue above wakes every parked task and lets each re-test its predicate,
 * which is all b1nix's own code has ever needed. Imported drivers need the other
 * half of Linux's interface: a waiter that is a *callback* rather than a parked
 * task, linked on the queue, invoked with the wake's key. i915's software fences
 * are built on it — a fence signals by walking its queue and calling each
 * entry's function, and the entries are what chain one fence to the next.
 *
 * So the shapes are upstream's, down to the member names: imported code walks
 * `wq->head` with list_for_each_entry and reads `entry`, `flags`, `private` and
 * `func` directly. Renaming any of them would mean editing that code, which is
 * the one thing this layer exists not to do.
 */
struct wait_queue_entry;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq, unsigned mode,
                                 int flags, void *key);

struct wait_queue_entry {
	unsigned int flags;
	void *private;
	wait_queue_func_t func;
	struct list_head entry;
};

#ifndef LKPI_WAIT_QUEUE_ENTRY_T_DEFINED
#define LKPI_WAIT_QUEUE_ENTRY_T_DEFINED
typedef struct wait_queue_entry wait_queue_entry_t;
#endif

#define WQ_FLAG_EXCLUSIVE 0x01
#define WQ_FLAG_WOKEN     0x02

struct wait_queue_head {
	volatile u64 wakeups; /* wake_up calls; diagnostics and self-test */
	volatile u32 waiters; /* tasks currently parked or about to park */
	/* Guards `head`. Named `lock` because imported code takes it directly —
	 * i915's fence code holds it across a walk of the entry list. Spelled as
	 * the underlying struct rather than `spinlock_t`, which is the name
	 * <linux/spinlock.h> gives this same type — that header must not be a
	 * prerequisite of this one. */
	struct lkpi_spinlock lock;
	struct list_head head;
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


/* Entry management. add/remove take the queue's lock; the __-prefixed forms
 * expect the caller to hold it already, which is what imported code does when
 * it is walking the list at the same time. */
void add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
void remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
void __add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
void __add_wait_queue_entry_tail(struct wait_queue_head *wq,
                                 struct wait_queue_entry *e);
void __remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);

/* Wake the callback entries with a key, as well as the parked tasks. A NULL key
 * is what plain wake_up passes. */
void __wake_up(struct wait_queue_head *wq, unsigned mode, int nr, void *key);

static inline void init_waitqueue_entry(struct wait_queue_entry *e, void *task)
{ e->flags = 0; e->private = task; e->func = 0; INIT_LIST_HEAD(&e->entry); }

static inline void init_waitqueue_func_entry(struct wait_queue_entry *e,
                                             wait_queue_func_t func)
{ e->flags = 0; e->private = 0; e->func = func; INIT_LIST_HEAD(&e->entry); }

#endif
