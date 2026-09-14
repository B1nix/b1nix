/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_RADIX_TREE_H
#define LKPI_LINUX_RADIX_TREE_H

/*
 * The radix tree, which upstream is now the xarray wearing its old name.
 *
 * <linux/xarray.h> already carries the `radix_tree_*` spellings this tree needs
 * — insert, lookup, delete, empty, the slot iterator — because the DRM import
 * needed them first. This header exists because imported filesystem code
 * includes it by name, and adds only what the older interface has that the
 * newer one does not.
 */
#include <linux/xarray.h>

/* `struct radix_tree_root` is the wrapper <linux/xarray.h> already defines
 * around the array; the functions below take it rather than the bare xarray,
 * because that is the type imported code holds. */
struct radix_tree_root;

/*
 * Gang lookup: up to `max_items` entries at or after `first_index`, in index
 * order, returning how many were found. The count can be short without meaning
 * the end of the tree — callers loop, resuming from the last index seen.
 */
unsigned int radix_tree_gang_lookup(const struct radix_tree_root *root, void **results,
                                    unsigned long first_index,
                                    unsigned int max_items);
unsigned int radix_tree_gang_lookup_tag(const struct radix_tree_root *root,
                                        void **results,
                                        unsigned long first_index,
                                        unsigned int max_items,
                                        unsigned int tag);
void *radix_tree_tag_set(struct radix_tree_root *root, unsigned long index,
                         unsigned int tag);
void *radix_tree_tag_clear(struct radix_tree_root *root, unsigned long index,
                           unsigned int tag);
int radix_tree_tagged(const struct radix_tree_root *root, unsigned int tag);

/*
 * Upstream `radix_tree_preload` reserves nodes so a later insert under a
 * spinlock cannot fail for memory, and disables preemption until the matching
 * `_end`. b1nix's allocator does not fail that way — it panics when the heap
 * cannot grow — so there is nothing to reserve, but the pairing is kept because
 * callers are written around the non-preemptible region between them.
 */
int radix_tree_preload(gfp_t gfp_mask);
void radix_tree_preload_end(void);

#endif
