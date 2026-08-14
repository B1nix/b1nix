/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_INTERVAL_TREE_H
#define LKPI_INTERVAL_TREE_H

#include <lkpi/rbtree.h>
#include <lkpi/types.h>

/*
 * Interval trees — "which of these ranges cover this address?"
 *
 * The question a GPU driver keeps asking. A fault arrives with one address and
 * the driver has to find the buffer object mapped over it; an mmap has to know
 * which existing mappings its range would collide with; the GPU address
 * allocator has to find a hole. Ranges overlap, so an ordinary ordered tree
 * cannot answer it: sorting by start tells you nothing about how far any earlier
 * range extends.
 *
 * The fix is one extra field. Each node remembers the largest `last` anywhere in
 * its subtree, so a query can look at a child and know, without descending, that
 * nothing under it reaches the address being asked about. That prunes the search
 * to the nodes that can actually match instead of the whole tree.
 *
 * The field is derived, so it must be restored whenever the tree's shape
 * changes — which is what the rbtree's augmentation hook is for. An
 * implementation that only maintains it on insert looks correct until a
 * rebalance moves a subtree, after which queries silently start missing
 * overlaps; that is the bug this is written to avoid, and the self-test checks
 * every node's field against a value recomputed from scratch.
 *
 * Intervals are closed: [start, last], so a one-byte range has start == last.
 * Duplicate and identical ranges are allowed — several objects may cover the
 * same address.
 *
 * Nothing here allocates or sleeps.
 */

struct interval_tree_node {
	struct rb_node rb;
	u64 start;        /* first address in the range */
	u64 last;         /* last address in the range, inclusive */
	u64 subtree_last; /* largest `last` in this node's subtree; derived */
};

struct interval_tree_root {
	struct rb_root rb_root;
};

static inline void interval_tree_init(struct interval_tree_root *root)
{
	rb_root_init(&root->rb_root);
}

static inline int interval_tree_empty(const struct interval_tree_root *root)
{
	return RB_EMPTY_ROOT(&root->rb_root);
}

/* `node->start` and `node->last` must be set; last >= start. */
void interval_tree_insert(struct interval_tree_node *node,
                          struct interval_tree_root *root);

void interval_tree_remove(struct interval_tree_node *node,
                          struct interval_tree_root *root);

/*
 * First node overlapping [start, last], then the next one, in start order.
 *
 *   for (n = interval_tree_iter_first(root, s, l); n;
 *        n = interval_tree_iter_next(n, s, l))
 *
 * Returns NULL when nothing (further) overlaps.
 */
struct interval_tree_node *interval_tree_iter_first(
	struct interval_tree_root *root, u64 start, u64 last);
struct interval_tree_node *interval_tree_iter_next(
	struct interval_tree_node *node, u64 start, u64 last);

/*
 * Verify every node's subtree_last against a value recomputed from its actual
 * subtree. Returns 0 when the whole tree agrees, or the number of nodes that do
 * not. Exists because a stale aggregate produces wrong answers rather than
 * crashes, so it has to be looked for on purpose.
 */
usize interval_tree_check(const struct interval_tree_root *root);

/*
 * The augmentation the tree maintains, exposed so a caller that inserts through
 * the rbtree API directly — as drm_mm does — recomputes the same field this
 * implementation reads. Two different compute functions over one field is a
 * stale-aggregate bug waiting to happen.
 */
extern const struct rb_augment_ops lkpi_interval_augment;

/* The callback itself, so a generated augment object can name it in a static
 * initialiser — reading it out of the struct above is not a constant
 * expression and cannot appear in one. */
void lkpi_interval_compute(struct rb_node *rb);

#endif
