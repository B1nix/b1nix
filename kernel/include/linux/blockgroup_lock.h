/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BLOCKGROUP_LOCK_H
#define LKPI_LINUX_BLOCKGROUP_LOCK_H

#include <linux/spinlock.h>
#include <linux/cache.h>

/*
 * A small array of spinlocks, hashed by block-group number.
 *
 * ext4 has thousands of block groups and locking each one individually would
 * cost a lock per group; one lock for all of them serialises unrelated
 * allocations. So upstream hashes the group onto a fixed array, sized by CPU
 * count, and this keeps the same arrangement with a fixed size — b1nix's CPU
 * count is small and known, and a 16-entry table is 128 bytes.
 *
 * Collisions are correct, just slower: two groups sharing a lock serialise
 * against each other and against nothing else.
 */

#define NR_BG_LOCKS 16

struct bgl_lock {
	spinlock_t lock;
} ____cacheline_aligned_in_smp;

struct blockgroup_lock {
	struct bgl_lock locks[NR_BG_LOCKS];
};

static inline void bgl_lock_init(struct blockgroup_lock *bgl)
{
	int i;

	for (i = 0; i < NR_BG_LOCKS; i++)
		spin_lock_init(&bgl->locks[i].lock);
}

static inline spinlock_t *bgl_lock_ptr(struct blockgroup_lock *bgl,
                                       unsigned int block_group)
{
	return &bgl->locks[block_group & (NR_BG_LOCKS - 1)].lock;
}

#endif
