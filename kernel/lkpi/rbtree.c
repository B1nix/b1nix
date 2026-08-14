/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: red-black trees, with optional augmentation.
 *
 * Textbook red-black balancing, written against the invariants rather than
 * transcribed: the root is black, a red node has no red child, and every path
 * from a node to a leaf passes the same number of black nodes. Insert fixes the
 * one violation rb_link_node can create (a red node under a red parent) and
 * erase fixes the one removal can create (a path one black short).
 *
 * Parent pointers are stored plainly instead of packed into the colour word.
 * The packed form saves eight bytes per node and costs a mask on every parent
 * access; at the scale a GPU driver uses these — thousands of nodes, not
 * millions — the memory is not worth the class of bug that mask introduces.
 *
 * Every mutating routine takes an augmentation hook that may be NULL. The plain
 * entry points pass NULL and the compiler folds the checks away; the interval
 * tree passes its own, and that is the only reason its subtree maxima survive a
 * rebalance. Keeping one implementation rather than two means the balancing is
 * debugged once.
 */

#include <lkpi/rbtree.h>

static inline void rb_compute(const struct rb_augment_ops *aug,
                              struct rb_node *node)
{
	if (aug && node)
		aug->compute(node);
}

/* Recompute the derived field from `node` up to the root. Used after a
 * structural change, which is the only time an ancestor's aggregate can go
 * stale without a rotation having touched it. */
static void rb_propagate(const struct rb_augment_ops *aug, struct rb_node *node)
{
	if (!aug)
		return;
	while (node) {
		aug->compute(node);
		node = node->__rb_parent;
	}
}

static void rb_rotate_left(struct rb_node *x, struct rb_root *root,
                           const struct rb_augment_ops *aug)
{
	struct rb_node *y = x->rb_right;
	x->rb_right = y->rb_left;
	if (y->rb_left)
		y->rb_left->__rb_parent = x;
	y->__rb_parent = x->__rb_parent;
	if (!x->__rb_parent)
		root->rb_node = y;
	else if (x == x->__rb_parent->rb_left)
		x->__rb_parent->rb_left = y;
	else
		x->__rb_parent->rb_right = y;
	y->rb_left = x;
	x->__rb_parent = y;
	/* x sank, y rose: recompute the lower one first so the upper one sees a
	 * correct child. Nothing above y changed — it holds the same nodes the
	 * subtree held before. */
	rb_compute(aug, x);
	rb_compute(aug, y);
}

static void rb_rotate_right(struct rb_node *x, struct rb_root *root,
                            const struct rb_augment_ops *aug)
{
	struct rb_node *y = x->rb_left;
	x->rb_left = y->rb_right;
	if (y->rb_right)
		y->rb_right->__rb_parent = x;
	y->__rb_parent = x->__rb_parent;
	if (!x->__rb_parent)
		root->rb_node = y;
	else if (x == x->__rb_parent->rb_right)
		x->__rb_parent->rb_right = y;
	else
		x->__rb_parent->rb_left = y;
	y->rb_right = x;
	x->__rb_parent = y;
	rb_compute(aug, x);
	rb_compute(aug, y);
}

static int rb_is_black(const struct rb_node *n)
{
	/* A missing child counts as black — that is what makes the black-height
	 * invariant well defined at the leaves. */
	return !n || n->color == RB_BLACK;
}

static void rb_insert_color_aug(struct rb_node *node, struct rb_root *root,
                                const struct rb_augment_ops *aug)
{
	if (!node || !root)
		return;

	while (node != root->rb_node && node->__rb_parent &&
	       node->__rb_parent->color == RB_RED) {
		struct rb_node *parent = node->__rb_parent;
		struct rb_node *gparent = parent->__rb_parent;
		if (!gparent)
			break;

		if (parent == gparent->rb_left) {
			struct rb_node *uncle = gparent->rb_right;
			if (!rb_is_black(uncle)) {
				/* Red uncle: recolour and carry the problem up two levels. */
				parent->color = RB_BLACK;
				uncle->color = RB_BLACK;
				gparent->color = RB_RED;
				node = gparent;
				continue;
			}
			if (node == parent->rb_right) {
				/* Inner child: rotate it out so the next rotation fixes it. */
				rb_rotate_left(parent, root, aug);
				node = parent;
				parent = node->__rb_parent;
			}
			parent->color = RB_BLACK;
			gparent->color = RB_RED;
			rb_rotate_right(gparent, root, aug);
		} else {
			struct rb_node *uncle = gparent->rb_left;
			if (!rb_is_black(uncle)) {
				parent->color = RB_BLACK;
				uncle->color = RB_BLACK;
				gparent->color = RB_RED;
				node = gparent;
				continue;
			}
			if (node == parent->rb_left) {
				rb_rotate_right(parent, root, aug);
				node = parent;
				parent = node->__rb_parent;
			}
			parent->color = RB_BLACK;
			gparent->color = RB_RED;
			rb_rotate_left(gparent, root, aug);
		}
	}
	if (root->rb_node)
		root->rb_node->color = RB_BLACK;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	rb_insert_color_aug(node, root, 0);
}

void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                         const struct rb_augment_ops *aug)
{
	if (!node || !root)
		return;
	/* The new node's own field first, then every ancestor: the insert changed
	 * the set below each of them even though no rotation has happened yet. */
	rb_propagate(aug, node);
	rb_insert_color_aug(node, root, aug);
}

/*
 * Repair the black-height after a black node left the tree.
 *
 * `node` is the child that took the removed node's place and may be NULL, which
 * is why the parent is passed separately: a NULL node cannot name its own
 * parent, and the whole fixup is about the sibling on the other side of it.
 */
static void rb_erase_color(struct rb_node *node, struct rb_node *parent,
                           struct rb_root *root,
                           const struct rb_augment_ops *aug)
{
	while (rb_is_black(node) && node != root->rb_node && parent) {
		if (parent->rb_left == node) {
			struct rb_node *sib = parent->rb_right;
			if (sib && sib->color == RB_RED) {
				/* Red sibling: recolour and rotate to get a black one. */
				sib->color = RB_BLACK;
				parent->color = RB_RED;
				rb_rotate_left(parent, root, aug);
				sib = parent->rb_right;
			}
			if (!sib)
				break;
			if (rb_is_black(sib->rb_left) && rb_is_black(sib->rb_right)) {
				/* Sibling has nothing to lend: shorten its side too and move
				 * the deficit up to the parent. */
				sib->color = RB_RED;
				node = parent;
				parent = node->__rb_parent;
				continue;
			}
			if (rb_is_black(sib->rb_right)) {
				if (sib->rb_left)
					sib->rb_left->color = RB_BLACK;
				sib->color = RB_RED;
				rb_rotate_right(sib, root, aug);
				sib = parent->rb_right;
			}
			if (sib) {
				sib->color = parent->color;
				if (sib->rb_right)
					sib->rb_right->color = RB_BLACK;
			}
			parent->color = RB_BLACK;
			rb_rotate_left(parent, root, aug);
			node = root->rb_node;
			break;
		} else {
			struct rb_node *sib = parent->rb_left;
			if (sib && sib->color == RB_RED) {
				sib->color = RB_BLACK;
				parent->color = RB_RED;
				rb_rotate_right(parent, root, aug);
				sib = parent->rb_left;
			}
			if (!sib)
				break;
			if (rb_is_black(sib->rb_left) && rb_is_black(sib->rb_right)) {
				sib->color = RB_RED;
				node = parent;
				parent = node->__rb_parent;
				continue;
			}
			if (rb_is_black(sib->rb_left)) {
				if (sib->rb_right)
					sib->rb_right->color = RB_BLACK;
				sib->color = RB_RED;
				rb_rotate_left(sib, root, aug);
				sib = parent->rb_left;
			}
			if (sib) {
				sib->color = parent->color;
				if (sib->rb_left)
					sib->rb_left->color = RB_BLACK;
			}
			parent->color = RB_BLACK;
			rb_rotate_right(parent, root, aug);
			node = root->rb_node;
			break;
		}
	}
	if (node)
		node->color = RB_BLACK;
}

static void rb_erase_aug(struct rb_node *node, struct rb_root *root,
                         const struct rb_augment_ops *aug)
{
	if (!node || !root)
		return;

	struct rb_node *child;
	struct rb_node *parent;
	u32 color;

	if (!node->rb_left) {
		child = node->rb_right;
	} else if (!node->rb_right) {
		child = node->rb_left;
	} else {
		/* Two children: the in-order successor takes this node's place, keeping
		 * the ordering. It is the leftmost node of the right subtree and so has
		 * no left child, which reduces this to the one-child case. */
		struct rb_node *old = node;
		struct rb_node *succ = node->rb_right;
		while (succ->rb_left)
			succ = succ->rb_left;

		child = succ->rb_right;
		parent = succ->__rb_parent;
		color = succ->color;

		if (parent == old) {
			/* Successor is the removed node's own right child: it stays where
			 * it is and becomes the parent of the replacement subtree. */
			parent = succ;
		} else {
			if (child)
				child->__rb_parent = parent;
			parent->rb_left = child;
			succ->rb_right = old->rb_right;
			old->rb_right->__rb_parent = succ;
		}

		succ->__rb_parent = old->__rb_parent;
		succ->color = old->color;
		succ->rb_left = old->rb_left;
		old->rb_left->__rb_parent = succ;

		if (old->__rb_parent) {
			if (old->__rb_parent->rb_left == old)
				old->__rb_parent->rb_left = succ;
			else
				old->__rb_parent->rb_right = succ;
		} else {
			root->rb_node = succ;
		}

		/* The lowest node whose subtree changed is `parent`; everything from
		 * there to the root lost a node and has to be recomputed. */
		rb_propagate(aug, parent);
		if (color == RB_BLACK)
			rb_erase_color(child, parent, root, aug);
		return;
	}

	parent = node->__rb_parent;
	color = node->color;
	if (child)
		child->__rb_parent = parent;
	if (parent) {
		if (parent->rb_left == node)
			parent->rb_left = child;
		else
			parent->rb_right = child;
	} else {
		root->rb_node = child;
	}

	rb_propagate(aug, parent);
	if (color == RB_BLACK)
		rb_erase_color(child, parent, root, aug);
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
	rb_erase_aug(node, root, 0);
}

void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
                        const struct rb_augment_ops *aug)
{
	rb_erase_aug(node, root, aug);
}

struct rb_node *rb_first(const struct rb_root *root)
{
	if (!root || !root->rb_node)
		return 0;
	struct rb_node *n = root->rb_node;
	while (n->rb_left)
		n = n->rb_left;
	return n;
}

struct rb_node *rb_last(const struct rb_root *root)
{
	if (!root || !root->rb_node)
		return 0;
	struct rb_node *n = root->rb_node;
	while (n->rb_right)
		n = n->rb_right;
	return n;
}

struct rb_node *rb_next(const struct rb_node *node)
{
	if (!node)
		return 0;
	if (node->rb_right) {
		struct rb_node *n = node->rb_right;
		while (n->rb_left)
			n = n->rb_left;
		return n;
	}
	/* No right subtree: the successor is the first ancestor this node is in the
	 * left subtree of. */
	struct rb_node *n = (struct rb_node *)node;
	struct rb_node *p = n->__rb_parent;
	while (p && n == p->rb_right) {
		n = p;
		p = p->__rb_parent;
	}
	return p;
}

struct rb_node *rb_prev(const struct rb_node *node)
{
	if (!node)
		return 0;
	if (node->rb_left) {
		struct rb_node *n = node->rb_left;
		while (n->rb_right)
			n = n->rb_right;
		return n;
	}
	struct rb_node *n = (struct rb_node *)node;
	struct rb_node *p = n->__rb_parent;
	while (p && n == p->rb_left) {
		n = p;
		p = p->__rb_parent;
	}
	return p;
}

usize rb_count(const struct rb_root *root)
{
	usize n = 0;
	for (struct rb_node *it = rb_first(root); it; it = rb_next(it))
		n++;
	return n;
}

/* Returns the black height, or -1 if the subtree violates an invariant. */
static int rb_check_node(const struct rb_node *node)
{
	if (!node)
		return 1; /* the NULL leaf is black */

	if (node->color == RB_RED) {
		if (!rb_is_black(node->rb_left) || !rb_is_black(node->rb_right))
			return -1; /* red node with a red child */
	}
	if (node->rb_left && node->rb_left->__rb_parent != node)
		return -1; /* parent pointer disagrees with the child pointer */
	if (node->rb_right && node->rb_right->__rb_parent != node)
		return -1;

	int lh = rb_check_node(node->rb_left);
	int rh = rb_check_node(node->rb_right);
	if (lh < 0 || rh < 0 || lh != rh)
		return -1;
	return lh + (node->color == RB_BLACK ? 1 : 0);
}

int rb_check(const struct rb_root *root)
{
	if (!root || !root->rb_node)
		return 1;
	if (root->rb_node->color != RB_BLACK)
		return -1;
	if (root->rb_node->__rb_parent)
		return -1;
	return rb_check_node(root->rb_node);
}

void rb_replace_node(struct rb_node *victim, struct rb_node *new_node,
                     struct rb_root *root)
{
	if (!victim || !new_node || !root)
		return;

	/* Inherit the links and the colour wholesale. The caller guarantees the
	 * two compare equal, so the ordering is unchanged and no rebalancing is
	 * needed — which is exactly why this exists instead of erase+insert. */
	*new_node = *victim;

	if (victim->rb_left)
		victim->rb_left->__rb_parent = new_node;
	if (victim->rb_right)
		victim->rb_right->__rb_parent = new_node;

	if (victim->__rb_parent) {
		if (victim->__rb_parent->rb_left == victim)
			victim->__rb_parent->rb_left = new_node;
		else
			victim->__rb_parent->rb_right = new_node;
	} else {
		root->rb_node = new_node;
	}
}
