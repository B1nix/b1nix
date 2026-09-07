/* SPDX-License-Identifier: GPL-2.0-only */
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

/*
 * Insert with a comparison callback, rather than an open-coded descent.
 *
 * The callback returns true when `node` sorts before `parent`, which is the
 * opposite convention from a qsort comparator — getting it backwards builds a
 * tree that is internally consistent and in the wrong order, and every search
 * then misses. btrfs inserts its free-space entries this way.
 *
 * The `_cached` forms additionally keep a pointer to the leftmost node, so
 * "smallest entry" is O(1); the insert has to maintain it, which is what the
 * `leftmost` bookkeeping below does.
 */
static inline void rb_add(struct rb_node *node, struct rb_root *tree,
                          bool (*less)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		parent = *link;
		if (less(node, parent))
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(node, parent, link);
	rb_insert_color(node, tree);
}

static inline struct rb_node *
rb_add_cached(struct rb_node *node, struct rb_root_cached *tree,
              bool (*less)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_root.rb_node;
	struct rb_node *parent = NULL;
	bool leftmost = true;

	while (*link) {
		parent = *link;
		if (less(node, parent)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(node, parent, link);
	rb_insert_color_cached(node, tree, leftmost);
	return leftmost ? node : NULL;
}

/*
 * Insert unless an equal node is already there.
 *
 * Returns NULL when the node was inserted, and the EXISTING node when one
 * compared equal — so a caller can tell "added" from "already present" without
 * a second lookup. Getting that return backwards makes a duplicate insert look
 * like a success, which for btrfs's global root tree means two roots claiming
 * the same objectid.
 */
static inline struct rb_node *
rb_find_add(struct rb_node *node, struct rb_root *tree,
            int (*cmp)(struct rb_node *, const struct rb_node *))
{
	struct rb_node **link = &tree->rb_node;
	struct rb_node *parent = NULL;
	int c;

	while (*link) {
		parent = *link;
		c = cmp(node, parent);
		if (c < 0)
			link = &parent->rb_left;
		else if (c > 0)
			link = &parent->rb_right;
		else
			return parent;
	}
	rb_link_node(node, parent, link);
	rb_insert_color(node, tree);
	return NULL;
}

static inline struct rb_node *
rb_find(const void *key, const struct rb_root *tree,
        int (*cmp)(const void *key, const struct rb_node *))
{
	struct rb_node *node = tree->rb_node;

	while (node) {
		int c = cmp(key, node);

		if (c < 0)
			node = node->rb_left;
		else if (c > 0)
			node = node->rb_right;
		else
			return node;
	}
	return NULL;
}

#endif
