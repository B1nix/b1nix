/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_REFCOUNT_H
#define LKPI_LINUX_REFCOUNT_H
#include <linux/atomic.h>
#include <lkpi/kref.h>
/*
 * A reference count that refuses to wrap.
 *
 * The difference from a bare atomic is the saturation: an increment past the
 * maximum, or a decrement below zero, is a bug that has already happened, and
 * wrapping turns it into a use-after-free later. Saturating leaks instead,
 * which is the survivable failure.
 */
/* The counter itself lives in <lkpi/kref.h>, so a kref can contain one without
 * this header — imported code reaches through kref.refcount.refs and both
 * spellings have to name one object. */
typedef lkpi_refcount_t refcount_t;
#define REFCOUNT_INIT(n) { { (n) } }
/* Saturation is a panic here (see lkpi_refcount_bug), so the comment above
 * describes the intent; the effect is that a wrap never happens silently. */
static inline void refcount_set(refcount_t *r, int n) { __atomic_store_n(&r->refs.counter, n, __ATOMIC_RELAXED); }
static inline unsigned int refcount_read(const refcount_t *r)
{ return (unsigned int)__atomic_load_n(&r->refs.counter, __ATOMIC_ACQUIRE); }
static inline void refcount_inc(refcount_t *r)
{
	int old = __atomic_fetch_add(&r->refs.counter, 1, __ATOMIC_RELAXED);

	if (__builtin_expect(old <= 0 || old == 0x7fffffff, 0))
		lkpi_refcount_bug("refcount_inc", r, old);
}
static inline bool refcount_inc_not_zero(refcount_t *r)
{ return kref_get_unless_zero((struct kref *)r) != 0; }
static inline bool refcount_dec_and_test(refcount_t *r)
{
	int old = __atomic_fetch_sub(&r->refs.counter, 1, __ATOMIC_ACQ_REL);

	if (__builtin_expect(old <= 0, 0))
		lkpi_refcount_bug("refcount_dec_and_test", r, old);
	return old == 1;
}
/* refcount_dec must never take the count to zero: the release would be lost. */
static inline void refcount_dec(refcount_t *r)
{
	int old = __atomic_fetch_sub(&r->refs.counter, 1, __ATOMIC_RELAXED);

	if (__builtin_expect(old <= 1, 0))
		lkpi_refcount_bug("refcount_dec", r, old);
}
/* Decrement unless the count is 1: the fast half of the *_dec_and_lock
 * family, which must take the lock before the last reference goes. */
static inline bool refcount_dec_not_one(refcount_t *r)
{
	int c = __atomic_load_n(&r->refs.counter, __ATOMIC_RELAXED);

	while (c != 1) {
		if (__builtin_expect(c <= 0, 0))
			lkpi_refcount_bug("refcount_dec_not_one", r, c);
		if (__atomic_compare_exchange_n(&r->refs.counter, &c, c - 1, 1,
		                                __ATOMIC_RELEASE, __ATOMIC_RELAXED))
			return true;
	}
	return false;
}

/* refcount_dec_and_lock_irqsave() needs both a refcount and a spinlock; it
 * lives in <linux/spinlock.h>, which is the one of the two that may include the
 * other without a cycle. */

/* Drop a reference and take the mutex only if it hit zero, atomically with
 * respect to another caller doing the same — the same reasoning as
 * atomic_dec_and_mutex_lock in <linux/mutex.h>. */
struct mutex;
int refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock);

#endif
