/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_IDR_H
#define LKPI_IDR_H

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
	volatile int lock; /* a b1nix spinlock; opaque here, see <lkpi/env.h> */
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

/* Replace what an id points at; returns the previous pointer, or NULL if the
 * id was not allocated. */
void *idr_replace(struct idr *idr, void *ptr, u32 id);

/* Highest id handed out so far. Ids are not dense, so this — not the count —
 * is what bounds a walk. */
u32 idr_max_allocated(struct idr *idr);

/* Unbind `id`, returning the pointer that was bound (or NULL). */
void *idr_remove(struct idr *idr, u32 id);

/* Live entry count. */
u32 idr_count(struct idr *idr);

/* Walk every live (id, ptr) pair. The callback runs WITHOUT the idr lock held,
 * so it may sleep, but it must not add to or remove from this idr. Returns the
 * number of entries visited; a callback returning non-zero stops the walk. */
int idr_for_each(struct idr *idr, /* The id is signed, matching Linux, so a callback written for one works
 * unchanged under the other. */
                 int (*fn)(int id, void *ptr, void *data),
                 void *data);


/*
 * ida — an id allocator with no object attached, for numbering things whose
 * identity is the number itself (a connector index, a minor). It is an idr
 * whose stored pointer nobody reads, built on the same allocator rather than
 * beside it so the id-reuse behaviour callers depend on is the behaviour that
 * is already tested.
 */
struct ida {
	struct idr idr;
	u32 initialised;
};

void ida_init(struct ida *ida);
void ida_destroy(struct ida *ida);
/* Returns the allocated id, or a negative errno. */
int ida_alloc(struct ida *ida, gfp_t gfp);
int ida_alloc_max(struct ida *ida, unsigned int max, gfp_t gfp);
int ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max,
                    gfp_t gfp);
void ida_free(struct ida *ida, unsigned int id);

#endif
