/* SPDX-License-Identifier: MIT */
#ifndef LKPI_IDR_H
#define LKPI_IDR_H

#include <b1nix/spinlock.h>
#include <lkpi/types.h>

/*
 * idr — integer id to pointer map.
 *
 * DRM uses one of these per file for GEM handles and one per device for object
 * ids, so the operations that must be cheap are alloc, lookup and remove; the
 * table is small (tens to low thousands of live ids) and sparse in practice
 * only after churn.
 *
 * Implementation: a flat, geometrically-grown pointer array indexed by id,
 * with a rotating "next free" hint. That gives O(1) lookup and amortised O(1)
 * alloc, which is what the callers need; Linux's radix tree buys memory
 * density that a table this size does not need.
 *
 * All entry points take the idr's own spinlock and never sleep, so an idr may
 * be used from interrupt context.
 */

struct idr {
	void **slots;
	u32 capacity;
	u32 hint;     /* where the next linear search starts */
	u32 count;    /* live entries */
	u32 base;     /* lowest id this idr hands out */
	spinlock_t lock;
};

/* Initialise an idr that allocates ids starting at `base`. */
void idr_init_base(struct idr *idr, u32 base);
static inline void idr_init(struct idr *idr) { idr_init_base(idr, 0); }

/* Release the backing table. Does not free the stored pointers. */
void idr_destroy(struct idr *idr);

/* Allocate the lowest free id in [start, end) and bind `ptr` to it. `end` may
 * be 0 for "no upper bound". Returns the id, or -ENOSPC / -ENOMEM. `ptr` must
 * not be NULL — NULL is how a free slot is represented. */
int idr_alloc(struct idr *idr, void *ptr, u32 start, u32 end);

/* Bind `ptr` to exactly `id`. Returns 0, -EBUSY if taken, -ENOMEM on growth
 * failure. */
int idr_alloc_at(struct idr *idr, void *ptr, u32 id);

/* Pointer bound to `id`, or NULL. */
void *idr_find(struct idr *idr, u32 id);

/* Unbind `id`, returning the pointer that was bound (or NULL). */
void *idr_remove(struct idr *idr, u32 id);

/* Live entry count. */
u32 idr_count(struct idr *idr);

/* Walk every live (id, ptr) pair. The callback runs WITHOUT the idr lock held,
 * so it may sleep, but it must not add to or remove from this idr. Returns the
 * number of entries visited; a callback returning non-zero stops the walk. */
int idr_for_each(struct idr *idr, int (*fn)(u32 id, void *ptr, void *data),
                 void *data);

#endif
