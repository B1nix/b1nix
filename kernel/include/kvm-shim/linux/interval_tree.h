/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_LINUX_INTERVAL_TREE_H
#define KVM_SHIM_LINUX_INTERVAL_TREE_H
/*
 * Linux's interval tree over [start, last], on an rb_root_cached (M131: KVM
 * keeps its memslots by host address in one). lkpi's own interval tree
 * (<lkpi/interval_tree.h>) has the same function names with a different
 * root type, so these are the same API under their own symbols, instantiated
 * from <linux/interval_tree_generic.h> in kernel/lkpi/kvm_support.c.
 */
#include <linux/rbtree.h>

struct interval_tree_node {
	struct rb_node rb;
	unsigned long start;
	unsigned long last;
	unsigned long __subtree_last;
};

#define interval_tree_insert    linux_interval_tree_insert
#define interval_tree_remove    linux_interval_tree_remove
#define interval_tree_iter_first linux_interval_tree_iter_first
#define interval_tree_iter_next linux_interval_tree_iter_next

void interval_tree_insert(struct interval_tree_node *node, struct rb_root_cached *root);
void interval_tree_remove(struct interval_tree_node *node, struct rb_root_cached *root);
struct interval_tree_node *interval_tree_iter_first(struct rb_root_cached *root,
						    unsigned long start, unsigned long last);
struct interval_tree_node *interval_tree_iter_next(struct interval_tree_node *node,
						   unsigned long start, unsigned long last);

/* Replace old with new in place (same interval). */
static inline void interval_tree_replace(struct interval_tree_node *old,
					 struct interval_tree_node *new,
					 struct rb_root_cached *root)
{
	interval_tree_remove(old, root);
	interval_tree_insert(new, root);
}
#endif
