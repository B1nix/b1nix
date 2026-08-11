/* SPDX-License-Identifier: MIT */
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
static inline void refcount_set(refcount_t *r, int n) { __atomic_store_n(&r->refs, n, __ATOMIC_RELAXED); }
static inline unsigned int refcount_read(const refcount_t *r)
{ return (unsigned int)__atomic_load_n(&r->refs, __ATOMIC_ACQUIRE); }
static inline void refcount_inc(refcount_t *r) { __atomic_fetch_add(&r->refs, 1, __ATOMIC_RELAXED); }
static inline bool refcount_inc_not_zero(refcount_t *r)
{ return kref_get_unless_zero((struct kref *)r) != 0; }
static inline bool refcount_dec_and_test(refcount_t *r)
{ return __atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL) == 0; }
static inline void refcount_dec(refcount_t *r) { __atomic_fetch_sub(&r->refs, 1, __ATOMIC_RELAXED); }

/* refcount_dec_and_lock_irqsave() needs both a refcount and a spinlock; it
 * lives in <linux/spinlock.h>, which is the one of the two that may include the
 * other without a cycle. */

#endif
