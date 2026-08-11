/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_RBTREE_H
#define LKPI_LINUX_RBTREE_H
#include <lkpi/rbtree.h>
#include <linux/kernel.h>
/*
 * The cached root, its insert/erase helpers and the augmented forms all live in
 * <lkpi/rbtree.h>, so b1nix's own self-tests can drive the same code imported
 * code runs on. This header is the Linux-named front for them.
 */

#include <linux/rbtree_augmented.h>

#define rb_parent(r) ((r)->__rb_parent)

#define RB_ROOT ((struct rb_root){ 0 })
#define RB_ROOT_CACHED ((struct rb_root_cached){ { 0 }, 0 })
/* A node not in any tree points at itself. The member is __rb_parent here;
 * see <lkpi/rbtree.h> for why it carries the underscores. */
#define RB_EMPTY_NODE(node) ((node)->__rb_parent == (node))
#define RB_CLEAR_NODE(node) ((node)->__rb_parent = (node))

/*
 * Post-order iteration, for tearing a tree down.
 *
 * Every node is visited after both its children, which is the only order in
 * which a caller may free as it goes — any other order frees a node that the
 * walk still has to descend through. There is no rebalancing, because the tree
 * is being destroyed.
 */
struct rb_node *rb_first_postorder(const struct rb_root *root);
struct rb_node *rb_next_postorder(const struct rb_node *node);

#define rbtree_postorder_for_each_entry_safe(pos, n, root, field)             \
	for (pos = rb_entry_safe(rb_first_postorder(root), __typeof__(*pos), field); \
	     pos && ({ n = rb_entry_safe(rb_next_postorder(&pos->field),          \
	                                 __typeof__(*pos), field); 1; });         \
	     pos = n)

#endif
