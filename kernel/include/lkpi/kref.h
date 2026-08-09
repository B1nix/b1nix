/* SPDX-License-Identifier: MIT */
#ifndef LKPI_KREF_H
#define LKPI_KREF_H

#include <lkpi/types.h>

/*
 * kref — the refcount every DRM object is built on.
 *
 * The whole point of the type is that the last put, and only the last put, runs
 * the release function. That has to hold when two CPUs drop the final two
 * references at the same time, so the decrement and the "was I the last one"
 * answer must be a single atomic step; testing the count and then comparing it
 * to zero separately is the classic double-free.
 *
 * kref_get on a zero count is a bug and not a race to be tolerated: reaching
 * zero means release has run or is about to, so the object may already be
 * gone. Callers holding only a weak reference must use kref_get_unless_zero,
 * which reports the failure instead of resurrecting freed memory.
 *
 * No operation here sleeps, so a kref may be manipulated under a spinlock —
 * but the release function is called on the last put, and *that* may sleep, so
 * the final put must not happen with a spinlock held.
 */

/*
 * A saturating reference count. The nesting matches Linux's — a kref holds a
 * refcount_t which holds the counter — because imported code reaches through
 * both names (kref.refcount.refs) rather than only calling the API.
 */
typedef struct {
	volatile i32 refs;
} lkpi_refcount_t;

struct kref {
	lkpi_refcount_t refcount;
};

#define lkpi_kref_counter(kref) ((kref)->refcount.refs)

typedef void (*kref_release_t)(struct kref *kref);

static inline void kref_init(struct kref *kref)
{
	__atomic_store_n(&kref->refcount.refs, 1, __ATOMIC_RELEASE);
}

static inline i32 kref_read(const struct kref *kref)
{
	return __atomic_load_n(&kref->refcount.refs, __ATOMIC_ACQUIRE);
}

static inline void kref_get(struct kref *kref)
{
	__atomic_fetch_add(&kref->refcount.refs, 1, __ATOMIC_RELAXED);
}

/*
 * Take a reference only if the object is still alive. Returns 1 on success.
 *
 * The compare-exchange loop is what makes this safe against a concurrent final
 * put: if the count reaches zero between the load and the store, the exchange
 * fails and we report the object as gone rather than raising a dead count back
 * to one.
 */
static inline int kref_get_unless_zero(struct kref *kref)
{
	i32 old = __atomic_load_n(&kref->refcount.refs, __ATOMIC_ACQUIRE);
	while (old > 0) {
		if (__atomic_compare_exchange_n(&kref->refcount.refs, &old, old + 1, 1,
		                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return 1;
		/* old now holds the value that was actually there; retry. */
	}
	return 0;
}

/*
 * Drop a reference. Returns 1 if this call released the object, in which case
 * `release` has already run and the memory may be gone.
 *
 * The acquire fence on the zero path pairs with every other CPU's release-order
 * decrement, so the releasing CPU sees all writes made through the references
 * that were dropped before it.
 */
static inline int kref_put(struct kref *kref, kref_release_t release)
{
	if (__atomic_fetch_sub(&kref->refcount.refs, 1, __ATOMIC_ACQ_REL) == 1) {
		if (release)
			release(kref);
		return 1;
	}
	return 0;
}

/* Recover the containing object from its embedded kref. */
#define kref_container_of(ptr, type, member) \
	((type *)((char *)(ptr) - (usize)(&((type *)0)->member)))

#endif
