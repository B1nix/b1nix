/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_INTERVAL_TREE_GENERIC_H
#define LKPI_LINUX_INTERVAL_TREE_GENERIC_H

#include <linux/rbtree.h>

/*
 * A typed interval tree, generated over the caller's own struct.
 *
 * The caller names the members: the embedded rb_node, the field holding the
 * largest endpoint anywhere below a node, and the two accessors that read an
 * interval's first and last address. Everything here is expressed through those
 * names, which is the point — the nodes belong to the caller's struct and this
 * code never assumes anything about where in it they sit.
 *
 * That assumption is exactly what an earlier version of this header got wrong.
 * It forwarded to lkpi's own u64-keyed interval tree by casting the caller's
 * object to that tree's node type, on the theory that one implementation could
 * serve every instantiation. `struct drm_mm_node` begins with `color`, `start`,
 * `size` and `mm` — its rb_node is six fields in — so the erase path wrote
 * child and parent pointers over the allocator's own bookkeeping. It did not
 * fault: it quietly turned a node's `start` into a pointer, and the damage
 * surfaced later as a cycle between two unrelated nodes in a different tree.
 * A cast is not a layout guarantee, and there is no cheap version of this.
 *
 * Queries are the reason for the augmentation: the subtree maximum is what lets
 * a search skip a whole subtree instead of walking it, and it is only
 * trustworthy because the rbtree recomputes it through every rotation.
 */

#define INTERVAL_TREE_DEFINE(ITSTRUCT, ITRB, ITTYPE, ITSUBTREE, ITSTART,       \
                             ITLAST, ITSTATIC, ITPREFIX)                       \
	_Static_assert(sizeof(ITTYPE) <= sizeof(u64),                              \
	               #ITPREFIX ": key wider than u64 would truncate");           \
                                                                               \
	/* One node's maximum endpoint: its own, or a child's if that reaches      \
	 * further. Called on both nodes of every rotation and along every changed \
	 * path, which is what keeps it true after a rebalance. */                 \
	static void ITPREFIX##_compute(struct rb_node *rb)                         \
	{                                                                          \
		ITSTRUCT *node = rb_entry(rb, ITSTRUCT, ITRB);                         \
		ITTYPE max = ITLAST(node);                                             \
		if (rb->rb_left) {                                                     \
			ITTYPE v = rb_entry(rb->rb_left, ITSTRUCT, ITRB)->ITSUBTREE;       \
			if (v > max)                                                       \
				max = v;                                                       \
		}                                                                      \
		if (rb->rb_right) {                                                    \
			ITTYPE v = rb_entry(rb->rb_right, ITSTRUCT, ITRB)->ITSUBTREE;      \
			if (v > max)                                                       \
				max = v;                                                       \
		}                                                                      \
		node->ITSUBTREE = max;                                                 \
	}                                                                          \
	/* Not ITSTATIC: that expands to `static inline` for some callers, and an  \
	 * object cannot be inline. */                                             \
	static const struct rb_augment_ops ITPREFIX##_augment =                    \
		{ .compute = ITPREFIX##_compute };                                     \
                                                                               \
	ITSTATIC void ITPREFIX##_insert(ITSTRUCT *node,                            \
	                                struct rb_root_cached *root)               \
	{                                                                          \
		struct rb_node **link = &root->rb_root.rb_node;                        \
		struct rb_node *parent = 0;                                            \
		ITTYPE start = ITSTART(node), last = ITLAST(node);                     \
		bool leftmost = true;                                                  \
                                                                               \
		node->ITSUBTREE = last;                                                \
		while (*link) {                                                        \
			parent = *link;                                                    \
			ITSTRUCT *p = rb_entry(parent, ITSTRUCT, ITRB);                    \
			/* Widen on the way down: every node passed now has this           \
			 * interval below it. */                                           \
			if (p->ITSUBTREE < last)                                           \
				p->ITSUBTREE = last;                                           \
			if (start < ITSTART(p)) {                                          \
				link = &parent->rb_left;                                       \
			} else {                                                           \
				link = &parent->rb_right;                                      \
				leftmost = false;                                              \
			}                                                                  \
		}                                                                      \
		rb_link_node(&node->ITRB, parent, link);                               \
		rb_insert_augmented_cached(&node->ITRB, root, leftmost,                \
		                           &ITPREFIX##_augment);                       \
	}                                                                          \
                                                                               \
	ITSTATIC void ITPREFIX##_remove(ITSTRUCT *node,                            \
	                                struct rb_root_cached *root)               \
	{                                                                          \
		rb_erase_augmented_cached(&node->ITRB, root, &ITPREFIX##_augment);     \
	}                                                                          \
                                                                               \
	/* The leftmost node in this subtree overlapping [start, last], or NULL.   \
	 * The left child is entered only when something below it can reach        \
	 * `start`; that test is the whole value of the augmentation. */           \
	static ITSTRUCT *ITPREFIX##_subtree_search(ITSTRUCT *node, ITTYPE start,   \
	                                           ITTYPE last)                    \
	{                                                                          \
		while (1) {                                                            \
			if (node->ITRB.rb_left) {                                          \
				ITSTRUCT *left =                                               \
					rb_entry(node->ITRB.rb_left, ITSTRUCT, ITRB);              \
				if (start <= left->ITSUBTREE) {                                \
					node = left;                                               \
					continue;                                                  \
				}                                                              \
			}                                                                  \
			if (ITSTART(node) <= last) {                                       \
				if (start <= ITLAST(node))                                     \
					return node;                                               \
				if (node->ITRB.rb_right) {                                     \
					node = rb_entry(node->ITRB.rb_right, ITSTRUCT, ITRB);      \
					continue;                                                  \
				}                                                              \
			}                                                                  \
			return 0;                                                          \
		}                                                                      \
	}                                                                          \
                                                                               \
	ITSTATIC ITSTRUCT *ITPREFIX##_iter_first(struct rb_root_cached *root,      \
	                                         ITTYPE start, ITTYPE last)        \
	{                                                                          \
		ITSTRUCT *node;                                                        \
                                                                               \
		if (!root->rb_root.rb_node)                                            \
			return 0;                                                          \
		node = rb_entry(root->rb_root.rb_node, ITSTRUCT, ITRB);                \
		if (node->ITSUBTREE < start)                                           \
			return 0;                                                          \
		return ITPREFIX##_subtree_search(node, start, last);                   \
	}                                                                          \
                                                                               \
	ITSTATIC ITSTRUCT *ITPREFIX##_iter_next(ITSTRUCT *node, ITTYPE start,      \
	                                        ITTYPE last)                       \
	{                                                                          \
		struct rb_node *rb = node->ITRB.rb_right, *prev;                       \
                                                                               \
		while (1) {                                                            \
			/* A right subtree that can still reach `start` holds the next     \
			 * match; otherwise climb until this node is a left child, which   \
			 * is where the in-order successor lives. */                       \
			if (rb) {                                                          \
				ITSTRUCT *right = rb_entry(rb, ITSTRUCT, ITRB);                \
				if (start <= right->ITSUBTREE)                                 \
					return ITPREFIX##_subtree_search(right, start, last);      \
			}                                                                  \
			do {                                                               \
				rb = node->ITRB.__rb_parent;                                   \
				if (!rb)                                                       \
					return 0;                                                  \
				prev = &node->ITRB;                                            \
				node = rb_entry(rb, ITSTRUCT, ITRB);                           \
				rb = node->ITRB.rb_right;                                      \
			} while (prev == rb);                                              \
			/* Past the query's end: the tree is ordered by start, so no       \
			 * later node can overlap either. */                               \
			if (ITSTART(node) > last)                                          \
				return 0;                                                      \
			if (start <= ITLAST(node))                                         \
				return node;                                                   \
		}                                                                      \
	}

#endif
