/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: xarray.
 *
 * A 64-way radix tree. `height` is the number of levels below the root, so the
 * root's slots are selected by bits [6*height-1 .. 6*(height-1)] of the index
 * and the last level holds the values themselves. Height 0 means the array is
 * empty and has no root at all.
 *
 * Two invariants carry everything else, and both are checked by the self-test:
 * a node is present only if at least one of its slots is non-NULL, and `used`
 * always equals the number of non-NULL slots in that node. The first is what
 * makes xa_empty() true again after the last erase; the second is what makes
 * the erase path able to fold empty nodes away without walking them.
 */

#include <b1nix/errno.h>
#include <b1nix/spinlock.h>
#include <lkpi/xarray.h>

/* Largest index representable at a given height. Height 11 covers the whole
 * 64-bit range (11 * 6 = 66 bits), so it is the ceiling. */
#define XA_MAX_HEIGHT 11

static u64 xa_max_index(u32 height)
{
	if (height == 0)
		return 0;
	if (height >= XA_MAX_HEIGHT)
		return (u64)-1;
	return ((u64)1 << (height * XA_SHIFT)) - 1;
}

static u32 xa_slot_at(u64 index, u32 level)
{
	/* `level` counts down from the root: level == height selects with the top
	 * chunk of bits, level == 1 selects the value slot. */
	return (u32)((index >> ((level - 1) * XA_SHIFT)) & XA_MASK);
}

static struct xa_node *xa_node_alloc(void)
{
	return (struct xa_node *)lkpi_kcalloc(1, sizeof(struct xa_node),
	                                      GFP_KERNEL);
}

void xa_init(struct xarray *xa)
{
	if (!xa)
		return;
	xa->root = 0;
	xa->height = 0;
	xa->count = 0;
	xa->lock = SPINLOCK_INIT;
}

static void xa_free_subtree(struct xa_node *node, u32 level)
{
	if (!node)
		return;
	if (level > 1) {
		for (u32 i = 0; i < XA_SLOTS; i++)
			xa_free_subtree((struct xa_node *)node->slots[i], level - 1);
	}
	lkpi_kfree(node);
}

void xa_destroy(struct xarray *xa)
{
	if (!xa)
		return;
	u64 flags;
	spin_lock_irqsave((spinlock_t *)&xa->lock, &flags);
	struct xa_node *root = xa->root;
	u32 height = xa->height;
	xa->root = 0;
	xa->height = 0;
	xa->count = 0;
	spin_unlock_irqrestore((spinlock_t *)&xa->lock, flags);
	/* Freed outside the lock: kfree must not run with interrupts disabled any
	 * longer than it has to, and nothing can reach this subtree any more. */
	xa_free_subtree(root, height);
}

/* Add levels until `index` is representable. Each new level takes the old tree
 * as its slot 0, because every existing index has zeroes in the bits the new
 * level consumes. */
static int xa_grow(struct xarray *xa, u64 index)
{
	while (index > xa_max_index(xa->height)) {
		if (xa->height >= XA_MAX_HEIGHT)
			return -ENOMEM; /* unreachable for a 64-bit index; kept explicit */
		if (!xa->root) {
			struct xa_node *node = xa_node_alloc();
			if (!node)
				return -ENOMEM;
			xa->root = node;
			xa->height = 1;
			continue;
		}
		struct xa_node *node = xa_node_alloc();
		if (!node)
			return -ENOMEM;
		node->slots[0] = xa->root;
		node->used = 1;
		xa->root = node;
		xa->height++;
	}
	if (!xa->root) {
		struct xa_node *node = xa_node_alloc();
		if (!node)
			return -ENOMEM;
		xa->root = node;
		xa->height = 1;
	}
	return 0;
}

/* Drop levels whose only occupant is slot 0, and the root itself once empty.
 * Called with the lock held; frees are deferred to the caller's list. */
static void xa_shrink(struct xarray *xa, struct xa_node **freed, u32 *n_freed)
{
	while (xa->root && xa->height > 1 && xa->root->used == 1 &&
	       xa->root->slots[0]) {
		struct xa_node *old = xa->root;
		xa->root = (struct xa_node *)old->slots[0];
		xa->height--;
		freed[(*n_freed)++] = old;
	}
	if (xa->root && xa->root->used == 0) {
		freed[(*n_freed)++] = xa->root;
		xa->root = 0;
		xa->height = 0;
	}
}

int xa_store(struct xarray *xa, u64 index, void *entry)
{
	if (!xa)
		return -EINVAL;
	if (!entry) {
		xa_erase(xa, index);
		return 0;
	}

	u64 flags;
	spin_lock_irqsave((spinlock_t *)&xa->lock, &flags);

	int err = xa_grow(xa, index);
	if (err) {
		spin_unlock_irqrestore((spinlock_t *)&xa->lock, flags);
		return err;
	}

	struct xa_node *node = xa->root;
	for (u32 level = xa->height; level > 1; level--) {
		u32 slot = xa_slot_at(index, level);
		struct xa_node *child = (struct xa_node *)node->slots[slot];
		if (!child) {
			child = xa_node_alloc();
			if (!child) {
				/* Partial path left behind holds no entries, so it is
				 * harmless: it will be folded away by the next erase or by
				 * xa_destroy. Reporting the failure matters more than
				 * unwinding it. */
				spin_unlock_irqrestore((spinlock_t *)&xa->lock, flags);
				return -ENOMEM;
			}
			node->slots[slot] = child;
			node->used++;
		}
		node = child;
	}

	u32 slot = xa_slot_at(index, 1);
	if (!node->slots[slot]) {
		node->used++;
		xa->count++;
	}
	node->slots[slot] = entry;

	spin_unlock_irqrestore((spinlock_t *)&xa->lock, flags);
	return 0;
}

void *xa_load(struct xarray *xa, u64 index)
{
	if (!xa)
		return 0;
	u64 flags;
	spin_lock_irqsave((spinlock_t *)&xa->lock, &flags);

	void *found = 0;
	if (xa->root && index <= xa_max_index(xa->height)) {
		struct xa_node *node = xa->root;
		for (u32 level = xa->height; level > 1 && node; level--)
			node = (struct xa_node *)node->slots[xa_slot_at(index, level)];
		if (node)
			found = node->slots[xa_slot_at(index, 1)];
	}

	spin_unlock_irqrestore((spinlock_t *)&xa->lock, flags);
	return found;
}

void *xa_erase(struct xarray *xa, u64 index)
{
	if (!xa)
		return 0;

	/* Nodes emptied by this erase, freed after the lock is dropped. At most one
	 * per level on the way down plus the levels xa_shrink folds away. */
	struct xa_node *freed[2 * XA_MAX_HEIGHT + 2];
	u32 n_freed = 0;

	u64 flags;
	spin_lock_irqsave((spinlock_t *)&xa->lock, &flags);

	void *old = 0;
	if (xa->root && index <= xa_max_index(xa->height)) {
		/* Remember the path so an emptied node can be unhooked from its
		 * parent without a second descent. */
		struct xa_node *path[XA_MAX_HEIGHT];
		u32 slots[XA_MAX_HEIGHT];
		u32 depth = 0;

		struct xa_node *node = xa->root;
		for (u32 level = xa->height; level > 1 && node; level--) {
			u32 slot = xa_slot_at(index, level);
			path[depth] = node;
			slots[depth] = slot;
			depth++;
			node = (struct xa_node *)node->slots[slot];
		}

		if (node) {
			u32 slot = xa_slot_at(index, 1);
			old = node->slots[slot];
			if (old) {
				node->slots[slot] = 0;
				node->used--;
				xa->count--;

				/* Unhook every node the removal emptied, deepest first. */
				while (node->used == 0 && depth > 0) {
					depth--;
					struct xa_node *parent = path[depth];
					parent->slots[slots[depth]] = 0;
					parent->used--;
					freed[n_freed++] = node;
					node = parent;
				}
				xa_shrink(xa, freed, &n_freed);
			}
		}
	}

	spin_unlock_irqrestore((spinlock_t *)&xa->lock, flags);

	for (u32 i = 0; i < n_freed; i++)
		lkpi_kfree(freed[i]);
	return old;
}

int xa_empty(struct xarray *xa)
{
	if (!xa)
		return 1;
	u64 flags;
	spin_lock_irqsave((spinlock_t *)&xa->lock, &flags);
	int empty = (xa->count == 0);
	spin_unlock_irqrestore((spinlock_t *)&xa->lock, flags);
	return empty;
}

usize xa_count(struct xarray *xa)
{
	if (!xa)
		return 0;
	u64 flags;
	spin_lock_irqsave((spinlock_t *)&xa->lock, &flags);
	usize n = xa->count;
	spin_unlock_irqrestore((spinlock_t *)&xa->lock, flags);
	return n;
}

/* Walk a subtree in slot order, which is index order because a higher slot at
 * any level means a larger index. `base` is the index of this subtree's slot 0. */
static int xa_walk(struct xa_node *node, u32 level, u64 base, xa_iter_fn fn,
                   void *data)
{
	if (!node)
		return 0;
	if (level == 1) {
		for (u32 i = 0; i < XA_SLOTS; i++) {
			if (!node->slots[i])
				continue;
			int r = fn(base + i, node->slots[i], data);
			if (r)
				return r;
		}
		return 0;
	}
	u64 span = (u64)1 << ((level - 1) * XA_SHIFT);
	for (u32 i = 0; i < XA_SLOTS; i++) {
		if (!node->slots[i])
			continue;
		int r = xa_walk((struct xa_node *)node->slots[i], level - 1,
		                base + (u64)i * span, fn, data);
		if (r)
			return r;
	}
	return 0;
}

int xa_for_each(struct xarray *xa, xa_iter_fn fn, void *data)
{
	if (!xa || !fn || !xa->root)
		return 0;
	/* Walked without the lock: the callback may block or call back into the
	 * caller's own locking, and holding an IRQ-off spinlock across it would
	 * break the never-sleep rule. Concurrent mutation during a walk is a
	 * caller error, as the header says. */
	return xa_walk(xa->root, xa->height, 0, fn, data);
}

/*
 * xa_init() with creation flags.
 *
 * The lock flavours are inert — the array locks internally — and the
 * allocating flavours only change where xa_alloc() starts looking, which is
 * recorded so it does. Everything else about the array is the same.
 */
void xa_init_flags(struct xarray *xa, unsigned int flags)
{
	xa_init(xa);
	if (xa)
		xa->alloc_base = (flags & XA_FLAGS_ALLOC1) ? 1u : 0u;
}

/*
 * The next present entry at or above *index.
 *
 * Re-descends from the root on every call rather than holding a cursor, which
 * is what makes erasing inside a walk safe here where upstream's batched cursor
 * would be left pointing into a freed node. The cost is a lookup per step
 * instead of an increment.
 */
void *xa_find_next(struct xarray *xa, u64 *index)
{
	if (!xa || !index)
		return 0;
	for (;;) {
		void *e = xa_load(xa, *index);
		if (e)
			return e;
		if (*index == (u64)-1)
			return 0;
		/* Nothing here; the array is sparse, so step and retry. The bound is
		 * the largest index the tree can hold at its current height — beyond
		 * that there is nothing to find. */
		if (*index >= lkpi_xa_max_index(xa))
			return 0;
		(*index)++;
	}
}

/* Largest index the tree can currently address. Above it every load misses, so
 * a walk that has passed it is finished. */
u64 lkpi_xa_max_index(struct xarray *xa)
{
	if (!xa || xa->height == 0)
		return 0;
	return (xa->height * XA_SHIFT >= 64)
	       ? (u64)-1
	       : (1ull << (xa->height * XA_SHIFT)) - 1;
}

/*
 * Store at the first free index in [limit.min, limit.max], reporting it.
 *
 * A linear search from the allocating base. That is O(n) in the number of
 * entries where upstream's marks free space in the tree — acceptable because
 * the callers here allocate context and engine ids, which number in the tens.
 */
int xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit,
             gfp_t gfp)
{
	u64 i;

	(void)gfp;
	if (!xa || !id)
		return -EINVAL;
	for (i = limit.min > xa->alloc_base ? limit.min : xa->alloc_base;
	     i <= limit.max; i++) {
		if (xa_load(xa, i))
			continue;
		int err = xa_store(xa, i, entry);
		if (err)
			return err;
		*id = (u32)i;
		return 0;
	}
	return -ENOSPC;
}
