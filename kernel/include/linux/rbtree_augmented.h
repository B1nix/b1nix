/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_RBTREE_AUGMENTED_H
#define LKPI_LINUX_RBTREE_AUGMENTED_H

#include <linux/rbtree.h>

/*
 * Generating an augmentation that keeps a subtree maximum.
 *
 * Callers give the field to maintain and how to compute a node's own
 * contribution; the macro emits the compute callback the tree calls on every
 * rotation and along every changed path. Writing that callback by hand is the
 * bug this exists to prevent — an augmentation that is only updated on insert
 * looks correct until a rebalance moves a subtree, after which queries quietly
 * return wrong answers.
 */
#define RB_DECLARE_CALLBACKS_MAX(rbstatic, rbname, rbstruct, rbfield, rbtype, \
                                 rbaugmented, rbcompute)                      \
	static void rbname##_compute(struct rb_node *rb)                          \
	{                                                                         \
		rbstruct *node = rb_entry(rb, rbstruct, rbfield);                     \
		rbtype max = rbcompute(node);                                         \
		if (rb->rb_left) {                                                    \
			rbtype v = rb_entry(rb->rb_left, rbstruct, rbfield)->rbaugmented; \
			if (v > max)                                                      \
				max = v;                                                      \
		}                                                                     \
		if (rb->rb_right) {                                                   \
			rbtype v = rb_entry(rb->rb_right, rbstruct, rbfield)->rbaugmented;\
			if (v > max)                                                      \
				max = v;                                                      \
		}                                                                     \
		node->rbaugmented = max;                                              \
	}                                                                         \
	rbstatic const struct rb_augment_ops rbname =                             \
		{ .compute = rbname##_compute };

#define RB_DECLARE_CALLBACKS(rbstatic, rbname, rbstruct, rbfield, rbaugmented, \
                             rbcompute)                                        \
	RB_DECLARE_CALLBACKS_MAX(rbstatic, rbname, rbstruct, rbfield, u64,         \
	                         rbaugmented, rbcompute)

/* The augmented erase, spelled the way imported code calls it. */
#define rb_erase_augmented(node, root, aug) rb_erase_augmented(node, root, aug)

#endif
