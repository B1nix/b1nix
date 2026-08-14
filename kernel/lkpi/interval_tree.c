/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: interval trees.
 *
 * See <lkpi/interval_tree.h> for what the structure answers and why the
 * derived field is the whole trick. This file is the descent: an ordinary
 * red-black tree keyed by `start`, plus the pruning rule that makes a query
 * logarithmic instead of linear.
 *
 * The rule, stated once because everything below follows from it: a subtree can
 * contain an overlap only if its largest endpoint reaches the start of the range
 * being queried. If it does not, the entire subtree is skipped without being
 * walked. And because the tree is ordered by `start`, once a node's own start is
 * past the end of the query, nothing to its right can match either.
 */

#include <lkpi/interval_tree.h>

static struct interval_tree_node *itn(struct rb_node *rb)
{
	return rb ? rb_entry(rb, struct interval_tree_node, rb) : 0;
}

static u64 subtree_last_of(struct rb_node *rb)
{
	/* Only ever called on a child known to exist. There is deliberately no
	 * "absent child returns 0" convention: 0 is a perfectly good endpoint, so a
	 * missing child would compare equal to a range ending at address 0 and the
	 * search would follow it into nothing. Every caller checks the pointer. */
	return itn(rb)->subtree_last;
}

/* Can this subtree contain anything reaching `start`? A missing subtree cannot,
 * which is a different statement from "its maximum is 0". */
static int subtree_reaches(struct rb_node *rb, u64 start)
{
	return rb && itn(rb)->subtree_last >= start;
}

/* Recompute one node's aggregate from itself and its two children. This is the
 * only place the field is ever written. */
void lkpi_interval_compute(struct rb_node *rb)
{
	struct interval_tree_node *node = itn(rb);
	u64 max = node->last;
	if (rb->rb_left && subtree_last_of(rb->rb_left) > max)
		max = subtree_last_of(rb->rb_left);
	if (rb->rb_right && subtree_last_of(rb->rb_right) > max)
		max = subtree_last_of(rb->rb_right);
	node->subtree_last = max;
}

const struct rb_augment_ops lkpi_interval_augment = {
	.compute = lkpi_interval_compute,
};
#define interval_augment lkpi_interval_augment

void interval_tree_insert(struct interval_tree_node *node,
                          struct interval_tree_root *root)
{
	if (!node || !root)
		return;

	struct rb_node **link = &root->rb_root.rb_node;
	struct rb_node *parent = 0;

	/* Ordinary ordered descent by start. Equal starts go right, so identical
	 * ranges coexist instead of one displacing the other. */
	while (*link) {
		parent = *link;
		struct interval_tree_node *this = itn(parent);
		/* Every node passed on the way down will contain the new range, so its
		 * aggregate can be widened here; the ancestors above the insertion
		 * point are then already correct before any rebalancing runs. */
		if (this->subtree_last < node->last)
			this->subtree_last = node->last;
		if (node->start < this->start)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}

	node->subtree_last = node->last;
	rb_link_node(&node->rb, parent, link);
	rb_insert_augmented(&node->rb, &root->rb_root, &interval_augment);
}

void interval_tree_remove(struct interval_tree_node *node,
                          struct interval_tree_root *root)
{
	if (!node || !root)
		return;
	rb_erase_augmented(&node->rb, &root->rb_root, &interval_augment);
}

/* Leftmost node in this subtree that overlaps [start, last], or NULL. */
static struct interval_tree_node *subtree_search(struct rb_node *rb, u64 start,
                                                 u64 last)
{
	while (rb) {
		struct interval_tree_node *node = itn(rb);

		if (subtree_reaches(rb->rb_left, start)) {
			/* Something on the left can still reach us; it would come first in
			 * start order, so it has to be tried before this node. */
			rb = rb->rb_left;
			continue;
		}
		if (node->start <= last) {
			if (node->last >= start)
				return node; /* this node itself overlaps */
			/* Its own range ends too early, but a node further right starts
			 * later and may extend far enough. */
			rb = rb->rb_right;
			continue;
		}
		/* This node starts past the end of the query, and everything to its
		 * right starts later still. */
		return 0;
	}
	return 0;
}

struct interval_tree_node *interval_tree_iter_first(
	struct interval_tree_root *root, u64 start, u64 last)
{
	if (!root || RB_EMPTY_ROOT(&root->rb_root) || start > last)
		return 0;
	if (!subtree_reaches(root->rb_root.rb_node, start))
		return 0; /* nothing in the tree reaches the query at all */
	return subtree_search(root->rb_root.rb_node, start, last);
}

struct interval_tree_node *interval_tree_iter_next(
	struct interval_tree_node *node, u64 start, u64 last)
{
	if (!node || start > last)
		return 0;

	struct rb_node *rb = node->rb.rb_right;
	struct rb_node *prev = &node->rb;

	for (;;) {
		/* Look into the right subtree first: it holds the nodes that come next
		 * in start order and are still below the current parent. */
		if (subtree_reaches(rb, start)) {
			struct interval_tree_node *found = subtree_search(rb, start, last);
			if (found)
				return found;
		}
		/* Otherwise climb, and take each ancestor we reach from the left — that
		 * is the next node in order that has not been examined. */
		for (;;) {
			struct rb_node *parent = prev->__rb_parent;
			if (!parent)
				return 0;
			if (parent->rb_left == prev) {
				struct interval_tree_node *up = itn(parent);
				if (up->start > last)
					return 0; /* past the end of the query, and so is the rest */
				if (up->last >= start)
					return up;
				rb = parent->rb_right;
				prev = parent;
				break;
			}
			prev = parent;
		}
	}
}

/* Recompute a subtree's true maximum endpoint, ignoring the stored field. */
static u64 subtree_max_recomputed(const struct rb_node *rb)
{
	if (!rb)
		return 0;
	const struct interval_tree_node *node =
		rb_entry_const(rb, struct interval_tree_node, rb);
	u64 max = node->last;
	u64 l = subtree_max_recomputed(rb->rb_left);
	u64 r = subtree_max_recomputed(rb->rb_right);
	if (l > max)
		max = l;
	if (r > max)
		max = r;
	return max;
}

static usize check_node(const struct rb_node *rb)
{
	if (!rb)
		return 0;
	const struct interval_tree_node *node =
		rb_entry_const(rb, struct interval_tree_node, rb);
	usize bad = (node->subtree_last != subtree_max_recomputed(rb)) ? 1 : 0;
	return bad + check_node(rb->rb_left) + check_node(rb->rb_right);
}

usize interval_tree_check(const struct interval_tree_root *root)
{
	if (!root)
		return 0;
	return check_node(root->rb_root.rb_node);
}
