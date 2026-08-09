/* SPDX-License-Identifier: MIT */
#ifndef LKPI_XARRAY_H
#define LKPI_XARRAY_H

#include <lkpi/types.h>

/*
 * xarray — a sparse map from a 64-bit index to a pointer.
 *
 * Where idr hands out small dense ids and remembers what they mean, an xarray is
 * told the index. Drivers use it when the key comes from somewhere else and is
 * nowhere near dense: a GPU address, a handle from userspace, a fence sequence
 * number. A flat array would need the whole span; a hash table would answer
 * lookups but not "walk these in index order", which is what teardown and
 * eviction want.
 *
 * The shape is a radix tree with 64 slots per node, so a key is consumed six
 * bits at a time and a lookup is a handful of array indexings with no
 * comparisons and no rebalancing. The tree grows in height only as far as the
 * largest index in use, so an array holding a few small indices costs one node
 * no matter how big the key type is.
 *
 * Empty nodes are freed on erase. That is what keeps xa_empty() honest and stops
 * a long-lived array from accumulating the skeleton of every index it ever held.
 *
 * Storing NULL is the same as erasing: there is no way to tell a stored NULL
 * from an absent entry, so the API does not pretend to.
 *
 * xa_store allocates and may fail, returning -ENOMEM. Nothing here sleeps, so an
 * xarray may be used under a spinlock; the internal lock makes concurrent
 * stores and lookups safe on their own, but a read-modify-write across two calls
 * still needs the caller's own lock.
 */

#define XA_SHIFT 6
#define XA_SLOTS (1u << XA_SHIFT) /* 64 */
#define XA_MASK  (XA_SLOTS - 1u)

struct xa_node {
	void *slots[XA_SLOTS]; /* child nodes above height 1, values at height 1 */
	u32 used;              /* non-NULL slots, so emptiness is O(1) */
};

struct xarray {
	struct xa_node *root;
	u32 height;   /* levels below the root; 0 means the array is empty */
	usize count;  /* entries stored */
	volatile int lock; /* a b1nix spinlock; opaque here, see <lkpi/lock.h> */
};

void xa_init(struct xarray *xa);

/* Free every node. Stored pointers belong to the caller and are not touched. */
void xa_destroy(struct xarray *xa);

/* Returns 0, or -ENOMEM if a node could not be allocated. Storing NULL erases.
 * Replacing an existing entry is not an error. */
int xa_store(struct xarray *xa, u64 index, void *entry);

/* The stored pointer, or NULL. */
void *xa_load(struct xarray *xa, u64 index);

/* Remove and return what was there, or NULL. */
void *xa_erase(struct xarray *xa, u64 index);

/* 1 when nothing is stored. */
int xa_empty(struct xarray *xa);

usize xa_count(struct xarray *xa);

/*
 * Walk every entry in ascending index order. The callback returns 0 to keep
 * going or non-zero to stop, and that value is returned from xa_for_each.
 *
 * The callback must not store into or erase from the array it is walking: the
 * walk holds no position it could re-find after the tree reshapes.
 */
typedef int (*xa_iter_fn)(u64 index, void *entry, void *data);
int xa_for_each(struct xarray *xa, xa_iter_fn fn, void *data);

#endif
