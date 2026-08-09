/* SPDX-License-Identifier: MIT */
#ifndef LKPI_RBTREE_H
#define LKPI_RBTREE_H

#include <lkpi/types.h>

/*
 * Red-black trees.
 *
 * The DRM core keeps its address spaces in one of these: every GEM object's
 * offset lives in a tree so a fault can find the object covering an address, and
 * the GPU address allocator keeps its free ranges the same way. Both want
 * ordered iteration and worst-case logarithmic insert and erase, which is what a
 * balanced tree gives and a hash table does not.
 *
 * The node carries no key. Callers embed `struct rb_node` in their own object,
 * do the ordered descent themselves (that is the only place the key's type is
 * known), and hand the insertion point back through rb_link_node. This is why
 * one tree implementation serves objects keyed by offset, by address and by
 * stamp without generics or callbacks.
 *
 * Nothing here allocates, and nothing here sleeps: a tree may be manipulated
 * under a spinlock, which is what the fault path needs.
 */

#define RB_RED   0u
#define RB_BLACK 1u

/* Members carry Linux's spellings (rb_left/rb_right/rb_parent) because
 * imported code reaches into them directly — drm_mm walks the tree by hand. */
struct rb_node {
	struct rb_node *rb_left;
	struct rb_node *rb_right;
	/* Named with the underscores Linux uses, because imported code reads the
	 * parent through an rb_parent() macro and a member by that name would be
	 * rewritten by it. */
	struct rb_node *__rb_parent;
	u32 color;
};

struct rb_root {
	struct rb_node *rb_node;
};

#define RB_ROOT_INIT ((struct rb_root){ 0 })

/*
 * The "cached" root keeps a pointer to the leftmost node, so the common
 * "smallest element" query costs nothing. It lives here rather than beside the
 * Linux-named front because b1nix's own self-tests drive it: a stale leftmost
 * is exactly the failure that makes an emptied tree look non-empty to a
 * caller's "does anything fit" guard, and that has to be testable from this
 * side of the boundary.
 *
 * Layout is Linux's — the plain root first — because imported code reads
 * rb_leftmost directly and casts between the two.
 */
struct rb_root_cached {
	struct rb_root rb_root;
	struct rb_node *rb_leftmost;
};

#define RB_ROOT_CACHED_INIT ((struct rb_root_cached){ { 0 }, 0 })

static inline void rb_root_init(struct rb_root *root)
{
	root->rb_node = 0;
}

static inline int RB_EMPTY_ROOT(const struct rb_root *root)
{
	return root->rb_node == 0;
}

/*
 * Attach `node` below `parent` at the slot `link` points at, then recolour.
 *
 * The caller's descent loop ends holding the address of the NULL child pointer
 * where the node belongs; passing that address is what lets this function splice
 * the node in without repeating the comparison.
 */
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                                struct rb_node **link)
{
	node->__rb_parent = parent;
	node->rb_left = 0;
	node->rb_right = 0;
	node->color = RB_RED;
	*link = node;
}

/* Restore the invariants after rb_link_node. Always follows it. */
void rb_insert_color(struct rb_node *node, struct rb_root *root);

/* Remove `node`. The node's own links are left stale; the caller owns it. */
void rb_erase(struct rb_node *node, struct rb_root *root);

/*
 * Augmentation: keeping a field derived from a node's whole subtree.
 *
 * An interval tree stores, at every node, the largest endpoint anywhere below
 * it, and that is what lets a query skip a subtree instead of walking it. The
 * field is only trustworthy if it survives rebalancing, and rebalancing is
 * exactly where it breaks: a rotation moves subtrees between two nodes, so both
 * of their aggregates change even though no key did.
 *
 * `compute` recalculates one node's field from that node and its two children.
 * The tree calls it on both nodes of every rotation, bottom node first, and
 * along the path from any structural change up to the root. A rotation does not
 * alter the set of nodes below the subtree's new root, so once those two are
 * fixed, everything above them is already correct — which is why this is
 * cheaper than recomputing a path per rotation.
 */
struct rb_augment_ops {
	void (*compute)(struct rb_node *node);
};

void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                         const struct rb_augment_ops *aug);
void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
                        const struct rb_augment_ops *aug);

/* Ordered traversal. rb_first returns NULL for an empty tree; rb_next returns
 * NULL past the last node. */
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);

/* Put `new_node` where `victim` was, inheriting its links and colour. The
 * caller guarantees the two compare equal, so no rebalancing is needed. */
void rb_replace_node(struct rb_node *victim, struct rb_node *new_node,
                     struct rb_root *root);

/* Nodes in the tree. Walks it, so O(n) — for tests and diagnostics. */
usize rb_count(const struct rb_root *root);

/* ── the cached root ─────────────────────────────────────────────
 *
 * Every one of these keeps rb_leftmost honest: it is set on an insert that went
 * leftmost, and recomputed — before the erase, while the node's links are still
 * good — when the node it points at leaves.
 */

static inline void rb_root_cached_init(struct rb_root_cached *root)
{
	root->rb_root.rb_node = 0;
	root->rb_leftmost = 0;
}

static inline struct rb_node *rb_first_cached(const struct rb_root_cached *root)
{
	return root->rb_leftmost;
}

/* `leftmost` is the caller's descent telling us it never went right. */
static inline void rb_insert_color_cached(struct rb_node *node,
                                          struct rb_root_cached *root,
                                          int leftmost)
{
	if (leftmost)
		root->rb_leftmost = node;
	rb_insert_color(node, &root->rb_root);
}

static inline void rb_insert_augmented_cached(struct rb_node *node,
                                              struct rb_root_cached *root,
                                              int leftmost,
                                              const struct rb_augment_ops *aug)
{
	if (leftmost)
		root->rb_leftmost = node;
	rb_insert_augmented(node, &root->rb_root, aug);
}

static inline void rb_erase_cached(struct rb_node *node,
                                   struct rb_root_cached *root)
{
	/* Recompute rather than guess: a leftmost pointer left aimed at an erased
	 * node is a use-after-free waiting to be dereferenced. */
	if (root->rb_leftmost == node)
		root->rb_leftmost = rb_next(node);
	rb_erase(node, &root->rb_root);
}

static inline void rb_erase_augmented_cached(struct rb_node *node,
                                             struct rb_root_cached *root,
                                             const struct rb_augment_ops *aug)
{
	if (root->rb_leftmost == node)
		root->rb_leftmost = rb_next(node);
	rb_erase_augmented(node, &root->rb_root, aug);
}

static inline void rb_replace_node_cached(struct rb_node *victim,
                                          struct rb_node *new_node,
                                          struct rb_root_cached *root)
{
	if (root->rb_leftmost == victim)
		root->rb_leftmost = new_node;
	rb_replace_node(victim, new_node, &root->rb_root);
}

/*
 * Verify the red-black invariants and return the tree's black height, or -1 if
 * any of them is broken: a red root, a red node with a red child, or two
 * root-to-leaf paths with different black counts.
 *
 * This exists because "the tree still returns the right answers" is not evidence
 * that the balancing is correct — an unbalanced but ordered tree passes every
 * lookup test and then degrades to a list under the access pattern that matters.
 * The self-test asserts the height stays logarithmic.
 */
int rb_check(const struct rb_root *root);

/* Like rb_entry, but yields NULL for a NULL node — so a walk can end without
 * the caller computing an offset from NULL. */
#define rb_entry_safe(ptr, type, member) \
	({ __typeof__(ptr) __p = (ptr); __p ? rb_entry(__p, type, member) : 0; })

#define rb_entry(ptr, type, member) \
	((type *)((char *)(ptr) - (usize)(&((type *)0)->member)))

/* Same, for a read-only walk: keeps the const instead of casting it away. */
#define rb_entry_const(ptr, type, member) \
	((const type *)((const char *)(ptr) - (usize)(&((type *)0)->member)))

#endif
