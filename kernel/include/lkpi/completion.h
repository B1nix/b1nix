/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_COMPLETION_H
#define LKPI_COMPLETION_H

#include <lkpi/types.h>

/*
 * completion — "wait until someone else says this finished".
 *
 * Built on b1nix's scheduler_wait_prepare / _commit pair rather than on
 * scheduler_block_on, because the two-phase API is what closes the lost-wakeup
 * race: the waiter publishes itself on the channel, re-checks the condition,
 * and only then parks. A completion signalled between the check and the park is
 * therefore not lost.
 *
 * `done` counts completions, so a complete() that arrives before anyone waits
 * is still consumed by the next wait_for_completion — same semantics as a
 * counting semaphore with an unbounded value, which is what callers assume.
 *
 * wait_for_completion() sleeps: never call it holding a spinlock.
 * complete() and complete_all() do not sleep and are safe from an interrupt
 * handler.
 */

struct completion {
	volatile u32 done;
	volatile u32 all;  /* set by complete_all: every future wait succeeds */
};

void init_completion(struct completion *c);
void reinit_completion(struct completion *c);

/* Consume one completion, sleeping until one is available. */
void wait_for_completion(struct completion *c);

/* As above with a deadline in scheduler ticks (10 ms each). Returns the
 * remaining ticks (>0) on success, 0 on timeout. */
u64 wait_for_completion_timeout(struct completion *c, u64 timeout_ticks);

/* Non-blocking: consume a completion if one is pending. Returns 1 if it did. */
int try_wait_for_completion(struct completion *c);

/* 1 when a wait would not block. */
int completion_done(struct completion *c);

/* Release exactly one waiter. */
void complete(struct completion *c);
/* Release every current and future waiter until reinit_completion(). */
void complete_all(struct completion *c);

#endif
