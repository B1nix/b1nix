/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: a boot-time check of the imported xarray.
 *
 * lib/xarray.c is Linux's own and is not ours to test for correctness in
 * general; what is checked here is that it RUNS on this kernel — node
 * allocation, RCU-freed nodes, the lock, marks and the xa_state cursor that
 * btrfs walks its extent buffers with. A shim fault in any of those shows up
 * as a wrong answer here instead of as a corrupted filesystem.
 */

#include <linux/xarray.h>
#include <linux/idr.h>
#include <linux/maple_tree.h>
#include <linux/slab.h>
#include <lkpi/types.h>

int lkpi_xarray_selftest(u64 *seen);
int lkpi_maple_selftest(u64 *seen);

#define XA_TEST_ENTRIES 200
#define XA_TEST_STRIDE  1000003ul /* prime: indices share no prefixes */

static u32 xa_test_values[XA_TEST_ENTRIES];

int lkpi_xarray_selftest(u64 *seen)
{
	DEFINE_XARRAY(xa);
	unsigned long index, prev = 0;
	unsigned long walked = 0;
	void *entry;
	int ok = 1;
	int i;

	if (!xa_empty(&xa) || xa_load(&xa, 0) || xa_load(&xa, 123456789ul))
		ok = 0;

	for (i = 0; i < XA_TEST_ENTRIES; i++) {
		xa_test_values[i] = 0x5A000000u + (u32)i;
		if (xa_err(xa_store(&xa, (unsigned long)i * XA_TEST_STRIDE,
				    &xa_test_values[i], GFP_KERNEL)))
			ok = 0;
	}
	for (i = 0; i < XA_TEST_ENTRIES; i++) {
		u32 *p = xa_load(&xa, (unsigned long)i * XA_TEST_STRIDE);

		if (p != &xa_test_values[i] || *p != 0x5A000000u + (u32)i)
			ok = 0;
		if (xa_load(&xa, (unsigned long)i * XA_TEST_STRIDE + 1))
			ok = 0;
	}

	/* Replacing returns the old entry. */
	if (xa_store(&xa, 0, &xa_test_values[1], GFP_KERNEL) != &xa_test_values[0])
		ok = 0;
	if (xa_store(&xa, 0, &xa_test_values[0], GFP_KERNEL) != &xa_test_values[1])
		ok = 0;

	/* The top of the index space. */
	if (xa_err(xa_store(&xa, ULONG_MAX, &xa_test_values[2], GFP_KERNEL)) ||
	    xa_load(&xa, ULONG_MAX) != &xa_test_values[2] ||
	    xa_erase(&xa, ULONG_MAX) != &xa_test_values[2])
		ok = 0;

	/* Ordered iteration visits each entry once. */
	xa_for_each(&xa, index, entry) {
		if (walked && index <= prev)
			ok = 0;
		prev = index;
		walked++;
	}
	if (walked != XA_TEST_ENTRIES)
		ok = 0;

	/* Marks through an xa_state cursor, the way btrfs tags its buffers:
	 * mark every third entry, then count them with a marked walk. */
	{
		XA_STATE(xas, &xa, 0);
		unsigned long marked = 0, expect = 0;

		for (i = 0; i < XA_TEST_ENTRIES; i += 3) {
			xa_set_mark(&xa, (unsigned long)i * XA_TEST_STRIDE, XA_MARK_0);
			expect++;
		}
		if (!xa_marked(&xa, XA_MARK_0) || xa_marked(&xa, XA_MARK_1))
			ok = 0;
		xas_lock_irq(&xas);
		xas_for_each_marked(&xas, entry, ULONG_MAX, XA_MARK_0) {
			if (xas.xa_index % (3 * XA_TEST_STRIDE))
				ok = 0;
			xas_clear_mark(&xas, XA_MARK_0);
			marked++;
		}
		xas_unlock_irq(&xas);
		if (marked != expect || xa_marked(&xa, XA_MARK_0))
			ok = 0;
	}

	/* Erase returns what was there, and emptying folds the tree away. */
	for (i = 0; i < XA_TEST_ENTRIES; i++) {
		if (xa_erase(&xa, (unsigned long)i * XA_TEST_STRIDE) != &xa_test_values[i])
			ok = 0;
		if (xa_erase(&xa, (unsigned long)i * XA_TEST_STRIDE))
			ok = 0;
	}
	if (!xa_empty(&xa))
		ok = 0;
	xa_destroy(&xa);

	/* The idr, which is the radix tree over the same nodes. */
	{
		DEFINE_IDR(idr);
		int id1 = idr_alloc(&idr, &xa_test_values[0], 1, 0, GFP_KERNEL);
		int id2 = idr_alloc(&idr, &xa_test_values[1], 1, 0, GFP_KERNEL);

		if (id1 != 1 || id2 != 2 || idr_find(&idr, 2) != &xa_test_values[1])
			ok = 0;
		if (idr_remove(&idr, 1) != &xa_test_values[0] || idr_find(&idr, 1))
			ok = 0;
		idr_destroy(&idr);
	}

	*seen = walked;
	return ok;
}

/*
 * The maple tree, which btrfs's lru cache sits on. Its nodes come from a cache
 * aligned to their own size -- the tree keeps a node's type in the low bits of
 * the pointer -- and upstream frees them with plain kfree(). Enough ranges are
 * stored to split nodes several levels deep, and the tree is emptied and
 * destroyed so that every node goes back through that free.
 */
#define MT_TEST_RANGES 600
#define MT_TEST_STEP   16ul

int lkpi_maple_selftest(u64 *seen)
{
	DEFINE_MTREE(mt);
	unsigned long index;
	unsigned long walked = 0;
	void *entry;
	int ok = 1;
	int i;

	for (i = 0; i < MT_TEST_RANGES; i++) {
		unsigned long first = (unsigned long)i * MT_TEST_STEP;

		if (mtree_store_range(&mt, first, first + MT_TEST_STEP / 2 - 1,
				      xa_mk_value(i), GFP_KERNEL))
			ok = 0;
	}
	for (i = 0; i < MT_TEST_RANGES; i++) {
		unsigned long first = (unsigned long)i * MT_TEST_STEP;

		if (mtree_load(&mt, first) != xa_mk_value(i) ||
		    mtree_load(&mt, first + MT_TEST_STEP / 2 - 1) != xa_mk_value(i) ||
		    mtree_load(&mt, first + MT_TEST_STEP / 2))
			ok = 0;
	}
	index = 0;
	mt_for_each(&mt, entry, index, ULONG_MAX) {
		if (entry != xa_mk_value(walked))
			ok = 0;
		walked++;
	}
	if (walked != MT_TEST_RANGES)
		ok = 0;
	/* Erase the odd ranges, which rebalances, then check what is left. */
	for (i = 1; i < MT_TEST_RANGES; i += 2)
		if (mtree_erase(&mt, (unsigned long)i * MT_TEST_STEP) != xa_mk_value(i))
			ok = 0;
	for (i = 0; i < MT_TEST_RANGES; i++)
		if (!!mtree_load(&mt, (unsigned long)i * MT_TEST_STEP) != !(i & 1))
			ok = 0;
	mtree_destroy(&mt);
	if (!mtree_empty(&mt))
		ok = 0;
	*seen = walked;
	return ok;
}
