/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_KMUTEX_H
#define B1NIX_KMUTEX_H

#include <b1nix/types.h>

/* A sleeping mutex for kernel-internal data: FreeBSD's sx/mutex(9) with
 * INVARIANTS, in place of the test-and-set words that spun on
 * scheduler_yield() and recorded nothing.
 *
 * The word holds the owner's token (the spinlock owner token, never 0) or 0.
 * Taking one recursively, releasing one the caller does not hold, and taking
 * one where sleeping is not allowed (a spinlock held, inside an interrupt
 * handler) all panic. A waiter sleeps on the mutex until it is released
 * instead of spinning through the scheduler. The holder is in a kernel
 * critical section for as long as it holds it -- a fatal signal is not
 * acted on mid-section -- and the acquisition order is checked by WITNESS
 * together with the spinlocks'. */
typedef struct {
    volatile int owner;
    volatile u32 waiters;
} kmutex_t;

#define KMUTEX_INIT { 0, 0 }

void kmutex_lock(kmutex_t *m);
int kmutex_trylock(kmutex_t *m);
void kmutex_unlock(kmutex_t *m);
/* sx_assert(SA_XLOCKED): the caller holds `m`. */
void kmutex_assert_held(kmutex_t *m);

#endif /* B1NIX_KMUTEX_H */
