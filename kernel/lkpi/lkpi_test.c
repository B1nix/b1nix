/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M99 linuxkpi self-test.
 *
 * Every check compares the layer's behaviour against something established
 * independently of it: pointers the test allocated itself, frames whose
 * physical addresses came straight from the page allocator, bytes written to a
 * file through the VFS, and data read back through a *different* mapping than
 * the one under test. A marker is printed only after the corresponding
 * operation ran and its result was verified.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/vfs.h>
#include <lkpi/lkpi.h>
#include <string.h>

static void lkpi_report(const char *name, int ok, u64 detail)
{
	console_write(ok ? "M99-SMOKE: ok " : "M99-SMOKE: FAIL ");
	console_write(name);
	if (!ok) {
		console_write(" detail=");
		console_write_dec(detail);
	}
	console_write("\n");
}

/* ── idr ────────────────────────────────────────────────────────── */

static int idr_walk_cb(int id, void *ptr, void *data)
{
	u32 *seen = data;
	(void)id;
	(void)ptr;
	(*seen)++;
	return 0;
}

static void test_idr(void)
{
	struct idr idr;
	idr_init_base(&idr, 1);

	/* 64 distinct objects, each identified by its own address. */
	static u32 objects[64];
	int ids[64];
	int ok = 1;
	for (int i = 0; i < 64; i++) {
		objects[i] = 0xAB0000u + (u32)i;
		ids[i] = idr_alloc(&idr, &objects[i], 0, 0);
		if (ids[i] < 0)
			ok = 0;
	}
	/* Ids must be unique and start at the configured base. */
	for (int i = 0; i < 64 && ok; i++) {
		if (ids[i] < 1)
			ok = 0;
		for (int j = i + 1; j < 64; j++)
			if (ids[i] == ids[j])
				ok = 0;
	}
	/* Lookup must return the exact pointer that was stored, checked by
	 * reading the value the test wrote into the object. */
	for (int i = 0; i < 64 && ok; i++) {
		u32 *p = idr_find(&idr, (u32)ids[i]);
		if (p != &objects[i] || *p != 0xAB0000u + (u32)i)
			ok = 0;
	}
	if (idr_count(&idr) != 64)
		ok = 0;

	u32 seen = 0;
	idr_for_each(&idr, idr_walk_cb, &seen);
	if (seen != 64)
		ok = 0;

	/* Removal returns the stored pointer and makes the id miss. */
	void *removed = idr_remove(&idr, (u32)ids[7]);
	if (removed != &objects[7] || idr_find(&idr, (u32)ids[7]) != 0 ||
	    idr_count(&idr) != 63)
		ok = 0;

	/* The freed id is the one handed out next (dense reuse). */
	int reused = idr_alloc(&idr, &objects[7], 0, 0);
	if (reused != ids[7])
		ok = 0;

	/* Explicit placement and its conflict case. */
	if (idr_alloc_at(&idr, &objects[0], 9999) != 0)
		ok = 0;
	if (idr_alloc_at(&idr, &objects[1], 9999) != -EBUSY)
		ok = 0;
	if (idr_find(&idr, 9999) != &objects[0])
		ok = 0;

	/* A bounded range must be honoured. */
	int bounded = idr_alloc(&idr, &objects[2], 5000, 5001);
	if (bounded != 5000)
		ok = 0;
	int full = idr_alloc(&idr, &objects[3], 5000, 5001);
	if (full != -ENOSPC)
		ok = 0;

	idr_destroy(&idr);
	lkpi_report("idr", ok, 0);
}

/* ── completion ─────────────────────────────────────────────────── */

struct comp_test {
	struct completion done;
	struct completion started;
	volatile u32 payload;
};

static struct comp_test g_comp;

static void comp_thread(void *arg)
{
	(void)arg;
	wait_for_completion(&g_comp.started);
	/* Publish the payload BEFORE completing: a waiter that returns from
	 * wait_for_completion must observe it. */
	g_comp.payload = 0x5EED1234u;
	complete(&g_comp.done);
	scheduler_exit_current(0);
}

static void test_completion(void)
{
	int ok = 1;
	init_completion(&g_comp.done);
	init_completion(&g_comp.started);
	g_comp.payload = 0;

	if (completion_done(&g_comp.done))
		ok = 0;
	if (try_wait_for_completion(&g_comp.done))
		ok = 0;

	if (kthread_create("lkpi-comp", comp_thread, 0) < 0) {
		lkpi_report("completion", 0, 1);
		return;
	}

	complete(&g_comp.started);
	u64 left = wait_for_completion_timeout(&g_comp.done, 500);
	if (left == 0)
		ok = 0;
	/* The ordering guarantee is the point: the payload the other thread wrote
	 * before complete() must be visible now. */
	if (g_comp.payload != 0x5EED1234u)
		ok = 0;
	/* The completion was consumed. */
	if (try_wait_for_completion(&g_comp.done))
		ok = 0;

	/* A timeout on a completion nobody signals must actually time out. */
	struct completion never;
	init_completion(&never);
	if (wait_for_completion_timeout(&never, 2) != 0)
		ok = 0;

	/* complete_all releases repeatedly until reinit. */
	struct completion all;
	init_completion(&all);
	complete_all(&all);
	if (!try_wait_for_completion(&all) || !try_wait_for_completion(&all))
		ok = 0;
	reinit_completion(&all);
	if (try_wait_for_completion(&all))
		ok = 0;

	lkpi_report("completion", ok, 0);
}

/* ── workqueue ──────────────────────────────────────────────────── */

#define WQ_ITEMS 8
struct wq_item {
	struct work_struct work;
	u32 index;
};
static struct wq_item g_wq_items[WQ_ITEMS];
static volatile u32 g_wq_order[WQ_ITEMS];
static volatile u32 g_wq_ran;
static struct delayed_work g_wq_delayed;
static volatile u32 g_wq_delayed_ran;
static volatile u64 g_wq_delayed_tick;

static void wq_handler(struct work_struct *w)
{
	struct wq_item *item = (struct wq_item *)w;
	u32 slot = g_wq_ran;
	if (slot < WQ_ITEMS)
		g_wq_order[slot] = item->index;
	g_wq_ran = slot + 1;
}

static void wq_delayed_handler(struct work_struct *w)
{
	(void)w;
	g_wq_delayed_tick = scheduler_get_ticks();
	g_wq_delayed_ran = 1;
}

static void test_workqueue(void)
{
	int ok = 1;
	struct workqueue_struct *wq = alloc_workqueue("lkpi-test", 0, 1);
	if (!wq) {
		lkpi_report("workqueue", 0, 1);
		return;
	}

	g_wq_ran = 0;
	for (u32 i = 0; i < WQ_ITEMS; i++) {
		INIT_WORK(&g_wq_items[i].work, wq_handler);
		g_wq_items[i].index = 0xF00u + i;
		g_wq_order[i] = 0;
		if (!queue_work(wq, &g_wq_items[i].work))
			ok = 0;
	}
	/* Re-queuing a pending item coalesces instead of duplicating. */
	if (queue_work(wq, &g_wq_items[0].work) != 0)
		ok = 0;

	flush_workqueue(wq);

	/* Exactly WQ_ITEMS handlers ran, in submission order, each exactly once. */
	if (g_wq_ran != WQ_ITEMS)
		ok = 0;
	for (u32 i = 0; i < WQ_ITEMS && ok; i++) {
		if (g_wq_order[i] != 0xF00u + i)
			ok = 0;
		/* Each item's own completion counter proves it ran exactly once — the
		 * coalesced re-queue above must not have produced a second run. */
		if (g_wq_items[i].work.seq != 1)
			ok = 0;
	}

	lkpi_report("workqueue", ok, g_wq_ran);

	/* Delayed work must not run before its deadline. */
	int dok = 1;
	u64 armed_at = scheduler_get_ticks();
	INIT_DELAYED_WORK(&g_wq_delayed, wq_delayed_handler);
	g_wq_delayed_ran = 0;
	g_wq_delayed_tick = 0;
	if (!queue_delayed_work(wq, &g_wq_delayed, 5))
		dok = 0;
	if (g_wq_delayed_ran)
		dok = 0;
	for (int i = 0; i < 200 && !g_wq_delayed_ran; i++)
		scheduler_sleep_ticks(1);
	if (!g_wq_delayed_ran)
		dok = 0;
	else if (g_wq_delayed_tick < armed_at + 5)
		dok = 0; /* fired early */

	/* Cancelling a still-armed item stops it from ever running. */
	static struct delayed_work cancelled;
	INIT_DELAYED_WORK(&cancelled, wq_delayed_handler);
	if (!queue_delayed_work(wq, &cancelled, 1000))
		dok = 0;
	if (!cancel_delayed_work(&cancelled))
		dok = 0;

	lkpi_report("workqueue-delayed", dok, 0);
	destroy_workqueue(wq);
}

/* ── scatterlist ────────────────────────────────────────────────── */

static void test_scatterlist(void)
{
	int ok = 1;
	const u32 npages = 8;
	u64 frames[8];

	/* Allocate page by page so the runs are whatever the frame allocator
	 * hands back — exactly how a discontiguous GEM object is built. */
	for (u32 i = 0; i < npages; i++) {
		frames[i] = pmm_alloc_frame();
		if (!frames[i])
			ok = 0;
	}
	if (!ok) {
		lkpi_report("scatterlist", 0, 1);
		return;
	}

	struct sg_table sgt;
	if (sg_alloc_table_from_pages(&sgt, frames, npages) < 0) {
		for (u32 i = 0; i < npages; i++)
			pmm_free_frame(frames[i]);
		lkpi_report("scatterlist", 0, 2);
		return;
	}

	if (sgt.total_bytes != (u64)npages * PAGE_SIZE)
		ok = 0;
	/* Coalescing invariant: the entry count must equal the number of maximal
	 * adjacent runs among the frames, computed here independently. */
	u32 expect_ents = 1;
	for (u32 i = 1; i < npages; i++)
		if (frames[i] != frames[i - 1] + PAGE_SIZE)
			expect_ents++;
	if (sgt.nents != expect_ents)
		ok = 0;
	if (sg_is_contiguous(&sgt) != (expect_ents == 1))
		ok = 0;

	/* Offset lookup must agree with the frame list for every page. */
	for (u32 i = 0; i < npages && ok; i++)
		if (sg_phys_at(&sgt, (u64)i * PAGE_SIZE) != frames[i])
			ok = 0;
	if (sg_phys_at(&sgt, sgt.total_bytes) != 0)
		ok = 0;

	/* Write a per-page pattern through the sg copy helper and read it back
	 * through the direct map — a different path than the one that wrote it. */
	u32 pattern[8];
	for (u32 i = 0; i < npages; i++)
		pattern[i] = 0x1000u + i;
	for (u32 i = 0; i < npages; i++)
		sg_copy_from_buffer(&sgt, (u64)i * PAGE_SIZE, &pattern[i], sizeof(u32));
	for (u32 i = 0; i < npages && ok; i++) {
		volatile u32 *page =
		    (volatile u32 *)(usize)(frames[i] + vmm_direct_map_base());
		if (page[0] != pattern[i])
			ok = 0;
	}
	/* And the reverse direction, across a run boundary. */
	u8 spill[16];
	memset(spill, 0, sizeof(spill));
	usize got = sg_copy_to_buffer(&sgt, PAGE_SIZE - 8, spill, sizeof(spill));
	if (got != sizeof(spill))
		ok = 0;
	else if (*(u32 *)(spill + 8) != pattern[1])
		ok = 0;

	sg_free_table(&sgt);
	for (u32 i = 0; i < npages; i++)
		pmm_free_frame(frames[i]);
	lkpi_report("scatterlist", ok, 0);
}

/* ── ioremap ────────────────────────────────────────────────────── */

static void test_ioremap(void)
{
	int ok = 1;
	u64 frame = pmm_alloc_frame();
	if (!frame) {
		lkpi_report("ioremap", 0, 1);
		return;
	}
	volatile u32 *direct = (volatile u32 *)(usize)(frame + vmm_direct_map_base());
	for (int i = 0; i < 8; i++)
		direct[i] = 0xDEAD0000u + (u32)i;

	volatile u32 *io = ioremap(frame, PAGE_SIZE);
	if (!io) {
		lkpi_report("ioremap", 0, 2);
		return;
	}
	/* Each failure carries its own detail code: "FAIL ioremap detail=0" named
	 * nothing, and the two things that were actually broken here (the mapping
	 * was the direct-map alias, and the WC attribute was absent) look identical
	 * in that message. */
	int detail = 0;
	/* A genuinely new mapping, not the direct-map alias handed back. */
	if ((usize)io == (usize)direct)
		{ ok = 0; detail = 3; }
	/* Reads through the device mapping must see what the direct map wrote. The
	 * two aliases have different memory types, so the cacheable one has to be
	 * written back before the uncached one can observe it. */
	cache_flush_range((const void *)direct, PAGE_SIZE);
	for (int i = 0; i < 8 && ok; i++)
		if (readl((const volatile void *)&io[i]) != 0xDEAD0000u + (u32)i)
			{ ok = 0; detail = 4; }
	/* And writes through it must be visible on the other side. */
	writel(0x600DBEEFu, (volatile void *)&io[3]);
	cache_flush_range((const void *)direct, PAGE_SIZE);
	if (ok && direct[3] != 0x600DBEEFu)
		{ ok = 0; detail = 5; }

	/* A write-combining mapping of the same frame must carry the WC page
	 * attribute in its installed PTE, verified from the page tables. */
	volatile u32 *wc = ioremap_wc(frame, PAGE_SIZE);
	if (!wc) {
		{ ok = 0; detail = 6; }
	} else {
		/* Ask the arch whether the installed entry carries the WC type:
		 * the encoding is a PAT slot on x86_64 and an MAIR slot on
		 * aarch64, and VMM_WC's bit positions mean something else
		 * entirely in an AArch64 descriptor (AP[1] and AttrIndx). */
		if (!paging_pte_is_wc(paging_leaf_pte((u64)(usize)wc)))
			{ ok = 0; detail = 7; }
		writel(0xCAFEF00Du, (volatile void *)&wc[5]);
		mb();
		cache_flush_range((const void *)direct, PAGE_SIZE);
		if (ok && direct[5] != 0xCAFEF00Du)
			{ ok = 0; detail = 8; }
	}

	lkpi_report("ioremap", ok, detail);
	/* The MMIO aliases are permanent, so the frame stays owned by them. */
}

/* ── dma-mapping ────────────────────────────────────────────────── */

static void test_dma(void)
{
	/* One bit per check, so a failure names the check that fell over instead of
	 * reporting a bare 0 (the M99 dma-mapping failure was un-triageable without
	 * this). All bits set == everything passed. */
	int ok = 0x1ff;
	dma_addr_t handle = 0;
	usize size = 3 * PAGE_SIZE;
	void *cpu = dma_alloc_coherent(0, size, &handle, 0);
	if (!cpu || !handle) {
		lkpi_report("dma-mapping", 0, 1);
		return;
	}
	/* The device address must be the physical address of the CPU pointer,
	 * cross-checked with the VMM's own translation. */
	if (vmm_virt_to_phys(cpu) != handle)
		ok &= ~(1 << 0);
	if (handle & (PAGE_SIZE - 1))
		ok &= ~(1 << 1);
	if (handle + size > dma_addressable_limit())
		ok &= ~(1 << 2);
	/* dma_alloc_coherent promises zeroed memory. */
	const u8 *bytes = cpu;
	for (usize i = 0; i < size; i += 512)
		if (bytes[i] != 0)
			ok &= ~(1 << 3);

	/* Round-trip an existing kernel buffer. */
	u32 *buf = kmalloc(256);
	if (!buf) {
		ok &= ~(1 << 4);
	} else {
		buf[0] = 0x11223344u;
		dma_addr_t h2 = dma_map_single(buf, 256, DMA_TO_DEVICE);
		if (h2 != vmm_virt_to_phys(buf))
			ok &= ~(1 << 5);
		/* The mapping must not have disturbed the data. */
		if (buf[0] != 0x11223344u)
			ok &= ~(1 << 6);
		dma_unmap_single(h2, 256, DMA_TO_DEVICE);
		kfree(buf);
	}

	/* An sg table maps entry-for-entry. */
	u64 frames[4];
	for (int i = 0; i < 4; i++)
		frames[i] = pmm_alloc_frame();
	struct sg_table sgt;
	if (sg_alloc_table_from_pages(&sgt, frames, 4) == 0) {
		if (dma_map_sg(&sgt, DMA_BIDIRECTIONAL) != sgt.nents)
			ok &= ~(1 << 7);
		dma_unmap_sg(&sgt, DMA_BIDIRECTIONAL);
		sg_free_table(&sgt);
	} else {
		ok &= ~(1 << 8);
	}
	for (int i = 0; i < 4; i++)
		if (frames[i])
			pmm_free_frame(frames[i]);

	dma_free_coherent(0, size, cpu, handle);
	lkpi_report("dma-mapping", ok == 0x1ff, ok ^ 0x1ff);
}

/* ── dma bounce buffers ─────────────────────────────────────────────
 *
 * A device whose address window does not cover the memory it is handed has, on
 * a machine with no IOMMU, exactly one remedy: copy through memory it can
 * reach. This drives that path with a mask deliberately set below the buffer's
 * own physical address, and checks the two things the copy exists for — that
 * the device sees what the CPU wrote, and that the CPU sees what the device
 * wrote back.
 */
static void test_dma_bounce(void)
{
	int ok = 0x3f;
	u32 *buf = kmalloc(PAGE_SIZE);
	if (!buf) {
		lkpi_report("dma-bounce", 0, 1);
		return;
	}
	for (usize i = 0; i < PAGE_SIZE / sizeof(u32); i++)
		buf[i] = 0xB0000000u ^ (u32)i;

	u64 phys = vmm_virt_to_phys(buf);
	if (!phys || phys < 2 * PAGE_SIZE) {
		/* Nothing below the buffer to bounce into. */
		kfree(buf);
		lkpi_report("dma-bounce", 0, 2);
		return;
	}

	/* A window that ends one page below this buffer: the buffer is out of
	 * reach by construction, whatever the allocator happened to hand out. */
	u64 mask = phys - 1;
	dma_addr_t h = dma_map_single_masked(buf, PAGE_SIZE, DMA_BIDIRECTIONAL,
	                                     mask);
	if (!h)
		ok &= ~(1 << 0);
	if (h && !dma_mapping_is_bounced(h))
		ok &= ~(1 << 1); /* it was NOT bounced — the mask was ignored */
	if (h && (h + PAGE_SIZE - 1) > mask)
		ok &= ~(1 << 2); /* bounced somewhere the device still cannot reach */

	if (h) {
		/* What the device would read must be what the CPU wrote. */
		const volatile u32 *dev_view =
		    (const volatile u32 *)(usize)(h + vmm_direct_map_base());
		for (usize i = 0; i < PAGE_SIZE / sizeof(u32); i += 64)
			if (dev_view[i] != (0xB0000000u ^ (u32)i)) {
				ok &= ~(1 << 3);
				break;
			}

		/* Now play the device: rewrite the bounce, then unmap. The caller's
		 * buffer must come back holding what the device left. */
		volatile u32 *dev_write = (volatile u32 *)(usize)(h + vmm_direct_map_base());
		for (usize i = 0; i < PAGE_SIZE / sizeof(u32); i++)
			dev_write[i] = 0xDE000000u ^ (u32)i;
		dma_unmap_single(h, PAGE_SIZE, DMA_BIDIRECTIONAL);

		for (usize i = 0; i < PAGE_SIZE / sizeof(u32); i += 64)
			if (buf[i] != (0xDE000000u ^ (u32)i)) {
				ok &= ~(1 << 4);
				break;
			}
		/* The mapping is gone: the handle must no longer be known. */
		if (dma_mapping_is_bounced(h))
			ok &= ~(1 << 5);
	}

	/* The same buffer with a window that does cover it must NOT be copied —
	 * bouncing when it is unnecessary is a bug of its own. */
	dma_addr_t direct = dma_map_single(buf, PAGE_SIZE, DMA_TO_DEVICE);
	if (direct != phys || dma_mapping_is_bounced(direct))
		ok &= ~(1 << 2);
	dma_unmap_single(direct, PAGE_SIZE, DMA_TO_DEVICE);

	kfree(buf);
	lkpi_report("dma-bounce", ok == 0x3f, ok ^ 0x3f);
}

/* An sg table whose runs are out of the device's reach is bounced as a whole:
 * one block below the mask, entries pointing into it in order. That is what a
 * device with a segment limit needs — the alternative, one allocation per run,
 * hands it more segments than the caller built. */
static void test_dma_bounce_sg(void)
{
	int ok = 0x1f;
	const u32 pages = 4;
	u64 frames[4];
	struct sg_table sgt;

	for (u32 i = 0; i < pages; i++) {
		frames[i] = pmm_alloc_frame();
		if (!frames[i]) {
			lkpi_report("dma-bounce-sg", 0, 1);
			return;
		}
	}
	if (sg_alloc_table_from_pages(&sgt, frames, pages) < 0) {
		for (u32 i = 0; i < pages; i++)
			pmm_free_frame(frames[i]);
		lkpi_report("dma-bounce-sg", 0, 2);
		return;
	}

	/* Fill each page with a value only that page carries, so a copy that lands
	 * at the wrong offset in the block is visible. */
	for (u32 i = 0; i < pages; i++) {
		volatile u32 *p = (volatile u32 *)(usize)(frames[i] + vmm_direct_map_base());
		for (usize w = 0; w < PAGE_SIZE / sizeof(u32); w++)
			p[w] = 0x51000000u ^ (i << 20) ^ (u32)w;
	}

	/* A window ending below the lowest run puts the whole table out of reach. */
	u64 lowest = frames[0];
	for (u32 i = 1; i < pages; i++)
		if (frames[i] < lowest)
			lowest = frames[i];
	if (lowest < 2 * PAGE_SIZE) {
		sg_free_table(&sgt);
		for (u32 i = 0; i < pages; i++)
			pmm_free_frame(frames[i]);
		lkpi_report("dma-bounce-sg", 0, 3);
		return;
	}
	u64 mask = lowest - 1;

	if (dma_map_sg_masked(&sgt, DMA_BIDIRECTIONAL, mask) != sgt.nents)
		ok &= ~(1 << 0);

	/* Every entry must address the one block, in order and end to end. */
	dma_addr_t base = sgt.sgl[0].dma_address;
	/* A failed mapping leaves no device address, and the checks below read and
	 * write through it — reporting the failure beats faulting the kernel on a
	 * null dereference, which is what an unmapped bounce pool used to do. */
	if (!base) {
		sg_free_table(&sgt);
		for (u32 i = 0; i < pages; i++)
			pmm_free_frame(frames[i]);
		lkpi_report("dma-bounce-sg", 0, 4);
		return;
	}
	if (!dma_mapping_is_bounced(base))
		ok &= ~(1 << 1);
	u64 off = 0;
	for (u32 i = 0; i < sgt.nents; i++) {
		if (sgt.sgl[i].dma_address != base + off)
			ok &= ~(1 << 2);
		if (sgt.sgl[i].dma_address + sgt.sgl[i].length - 1 > mask)
			ok &= ~(1 << 2);
		off += sgt.sgl[i].length;
	}

	/* The block must hold what the caller's pages held, at the right offsets. */
	{
		const volatile u32 *blk =
		    (const volatile u32 *)(usize)(base + vmm_direct_map_base());
		for (u32 i = 0; i < pages; i++) {
			usize w0 = (usize)i * (PAGE_SIZE / sizeof(u32));
			if (blk[w0] != (0x51000000u ^ (i << 20)) ||
			    blk[w0 + 17] != (0x51000000u ^ (i << 20) ^ 17u)) {
				ok &= ~(1 << 3);
				break;
			}
		}

		/* Play the device: rewrite the block, unmap, and require every page to
		 * have received its own slice back. */
		volatile u32 *w = (volatile u32 *)(usize)(base + vmm_direct_map_base());
		for (usize i = 0; i < ((usize)pages * PAGE_SIZE) / sizeof(u32); i++)
			w[i] = 0xD1000000u ^ (u32)i;
	}
	dma_unmap_sg(&sgt, DMA_BIDIRECTIONAL);

	for (u32 i = 0; i < pages; i++) {
		const volatile u32 *p =
		    (const volatile u32 *)(usize)(frames[i] + vmm_direct_map_base());
		usize base_word = (usize)i * (PAGE_SIZE / sizeof(u32));
		if (p[0] != (0xD1000000u ^ (u32)base_word) ||
		    p[9] != (0xD1000000u ^ (u32)(base_word + 9))) {
			ok &= ~(1 << 4);
			break;
		}
	}

	sg_free_table(&sgt);
	for (u32 i = 0; i < pages; i++)
		pmm_free_frame(frames[i]);
	lkpi_report("dma-bounce-sg", ok == 0x1f, ok ^ 0x1f);
}

/* The pool exists to make "no bounce buffer" mean "too many mappings in
 * flight" instead of "somebody else fragmented memory". Check that a mapping
 * the pool can serve is served from it, that the accounting moves with the
 * mapping, and that the frames come back. */
static void test_dma_bounce_pool(void)
{
	int ok = 0x3f;
	u64 pool_base = 0, pool_end = 0;
	if (dma_bounce_pool_range(&pool_base, &pool_end) != 0) {
		lkpi_report("dma-bounce-pool", 0, 1);
		return;
	}

	usize frames = 0, in_use0 = 0, peak0 = 0, maps0 = 0;
	dma_bounce_pool_stats(&frames, &in_use0, &peak0, &maps0);
	if (frames == 0 || pool_end <= pool_base)
		ok &= ~(1 << 0);

	u32 *buf = kmalloc(PAGE_SIZE);
	if (!buf) {
		lkpi_report("dma-bounce-pool", 0, 2);
		return;
	}
	buf[0] = 0xF00DF00Du;

	/* A window that ends at the top of the pool: the pool is inside it, this
	 * buffer (kmalloc'd, far above) is not — so the mapping must bounce, and
	 * must bounce into the pool. */
	dma_addr_t h = dma_map_single_masked(buf, PAGE_SIZE, DMA_BIDIRECTIONAL,
	                                     pool_end - 1);
	if (!h || !dma_mapping_is_bounced(h))
		ok &= ~(1 << 1);
	if (h && (h < pool_base || h >= pool_end))
		ok &= ~(1 << 2); /* served from the allocator while the pool was free */

	usize in_use1 = 0, peak1 = 0, maps1 = 0;
	dma_bounce_pool_stats(0, &in_use1, &peak1, &maps1);
	if (in_use1 != in_use0 + 1 || maps1 != maps0 + 1)
		ok &= ~(1 << 3);
	if (peak1 < in_use1)
		ok &= ~(1 << 4);

	if (h) {
		const volatile u32 *dev = (const volatile u32 *)(usize)(h + vmm_direct_map_base());
		if (dev[0] != 0xF00DF00Du)
			ok &= ~(1 << 5);
		dma_unmap_single(h, PAGE_SIZE, DMA_BIDIRECTIONAL);
	}

	usize in_use2 = 0, maps2 = 0;
	dma_bounce_pool_stats(0, &in_use2, 0, &maps2);
	if (in_use2 != in_use0 || maps2 != maps0)
		ok &= ~(1 << 3); /* the slot did not come back */

	kfree(buf);
	lkpi_report("dma-bounce-pool", ok == 0x3f, ok ^ 0x3f);
}

/* When one block for the whole table cannot be had, the mapping must still
 * happen — run by run. Fragmentation is injected rather than waited for: under
 * QEMU the allocator always finds a contiguous run, so the fallback would
 * otherwise never execute here. */
static void test_dma_bounce_sg_fallback(void)
{
	int ok = 0xf;
	const u32 pages = 3;
	u64 pool[6];
	u64 frames[3];
	struct sg_table sgt;

	/* The runs must not coalesce: a table that collapses to one entry has
	 * nothing for a per-run fallback to fall back to. Hold 2N frames, sort
	 * them and take every other one — sorted and distinct means no two chosen
	 * frames are adjacent, whatever order the allocator used, and the unchosen
	 * ones stay held so nothing can fill the gaps. */
	for (u32 i = 0; i < pages * 2; i++) {
		pool[i] = pmm_alloc_frame();
		if (!pool[i]) {
			lkpi_report("dma-bounce-sg-fallback", 0, 1);
			return;
		}
	}
	for (u32 i = 1; i < pages * 2; i++) {
		u64 key = pool[i];
		u32 j = i;
		while (j > 0 && pool[j - 1] > key) {
			pool[j] = pool[j - 1];
			j--;
		}
		pool[j] = key;
	}
	for (u32 i = 0; i < pages; i++)
		frames[i] = pool[i * 2];
	if (sg_alloc_table_from_pages(&sgt, frames, pages) < 0) {
		for (u32 i = 0; i < pages * 2; i++)
			pmm_free_frame(pool[i]);
		lkpi_report("dma-bounce-sg-fallback", 0, 2);
		return;
	}
	if (sgt.nents != pages) {
		/* Adjacent after all: the construction above is supposed to make this
		 * impossible, so say so rather than testing something else. */
		sg_free_table(&sgt);
		for (u32 i = 0; i < pages * 2; i++)
			pmm_free_frame(pool[i]);
		lkpi_report("dma-bounce-sg-fallback", 0, 3);
		return;
	}
	for (u32 i = 0; i < pages; i++) {
		volatile u32 *p = (volatile u32 *)(usize)(frames[i] + vmm_direct_map_base());
		p[0] = 0xFA000000u ^ i;
		p[5] = 0xFB000000u ^ i;
	}

	u64 lowest = frames[0];
	for (u32 i = 1; i < pages; i++)
		if (frames[i] < lowest)
			lowest = frames[i];

	dma_bounce_force_single_page(1);
	u32 mapped = dma_map_sg_masked(&sgt, DMA_BIDIRECTIONAL, lowest - 1);
	dma_bounce_force_single_page(0);

	if (mapped != sgt.nents)
		ok &= ~(1 << 0);

	/* Every run must have its own block. Addresses alone cannot show that —
	 * separately allocated blocks can land adjacent by chance — so ask the
	 * mapping how many blocks it is made of. */
	u32 blocks = dma_bounce_mapping_blocks(sgt.sgl[0].dma_address);
	if (blocks != sgt.nents)
		ok &= ~(1 << 2);
	for (u32 i = 0; i < sgt.nents; i++)
		if (sgt.sgl[i].dma_address + sgt.sgl[i].length - 1 > lowest - 1)
			ok &= ~(1 << 1);

	/* Data still has to round-trip through the scattered blocks. */
	for (u32 i = 0; i < sgt.nents; i++) {
		volatile u32 *blk =
		    (volatile u32 *)(usize)(sgt.sgl[i].dma_address + vmm_direct_map_base());
		if (blk[0] != (0xFA000000u ^ i))
			ok &= ~(1 << 3);
		blk[0] = 0xFC000000u ^ i;
	}
	dma_unmap_sg(&sgt, DMA_BIDIRECTIONAL);
	for (u32 i = 0; i < pages; i++) {
		const volatile u32 *p =
		    (const volatile u32 *)(usize)(frames[i] + vmm_direct_map_base());
		if (p[0] != (0xFC000000u ^ i))
			ok &= ~(1 << 3);
	}

	sg_free_table(&sgt);
	for (u32 i = 0; i < pages * 2; i++)
		pmm_free_frame(pool[i]);
	/* Report what was seen, not only which bit fell over: the block count and
	 * the entry count are the whole question here. */
	lkpi_report("dma-bounce-sg-fallback", ok == 0xf,
	            (u64)(ok ^ 0xf) | ((u64)blocks << 8) | ((u64)sgt.nents << 16));
}

/* ── request_firmware ───────────────────────────────────────────── */

static void test_firmware(void)
{
	int ok = 1;
	static const char blob[] =
	    "b1nix-lkpi-firmware-blob\x00\x01\x02\x03 trailing bytes";
	const usize blob_len = sizeof(blob) - 1;

	vfs_mkdir("/lib", 0755);
	vfs_mkdir("/lib/firmware", 0755);
	int fd = vfs_open_flags_mode("/lib/firmware/lkpi-test.bin",
	                             B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC,
	                             0644);
	if (fd < 0) {
		lkpi_report("request-firmware", 0, 1);
		return;
	}
	isize wrote = vfs_write(fd, blob, blob_len);
	vfs_close(fd);
	if (wrote != (isize)blob_len) {
		lkpi_report("request-firmware", 0, 2);
		return;
	}

	const struct firmware *fw = 0;
	if (request_firmware(&fw, "lkpi-test.bin", 0) != 0 || !fw) {
		lkpi_report("request-firmware", 0, 3);
		return;
	}
	/* Size and every byte must match what was written — including the
	 * embedded NUL, which a string-based loader would truncate at. */
	if (fw->size != blob_len)
		ok = 0;
	else if (memcmp(fw->data, blob, blob_len) != 0)
		ok = 0;
	release_firmware(fw);

	/* A missing blob must report ENOENT rather than an empty success. */
	const struct firmware *missing = (const struct firmware *)1;
	if (firmware_request_nowarn(&missing, "definitely-not-here.bin", 0) != -ENOENT ||
	    missing != 0)
		ok = 0;

	lkpi_report("request-firmware", ok, 0);
}

/* ── locks ──────────────────────────────────────────────────────── */

static struct lkpi_mutex g_mutex;
static volatile u32 g_mutex_counter;
static volatile int g_mutex_thread_done;

static void mutex_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < 200; i++) {
		lkpi_mutex_lock(&g_mutex);
		u32 v = g_mutex_counter;
		scheduler_yield();
		g_mutex_counter = v + 1;
		lkpi_mutex_unlock(&g_mutex);
	}
	g_mutex_thread_done = 1;
	scheduler_exit_current(0);
}

static void test_locks(void)
{
	int ok = 1;
	struct lkpi_spinlock sl;
	lkpi_spin_lock_init(&sl);
	lkpi_spin_lock(&sl);
	lkpi_spin_unlock(&sl);
	/* Re-acquirable after release: a leaked flags word would deadlock here. */
	lkpi_spin_lock(&sl);
	lkpi_spin_unlock(&sl);

	lkpi_mutex_init(&g_mutex);
	if (!lkpi_mutex_trylock(&g_mutex))
		ok = 0;
	if (lkpi_mutex_trylock(&g_mutex))
		ok = 0; /* already held: trylock must fail */
	if (!lkpi_mutex_is_locked_by_current(&g_mutex))
		ok = 0;
	lkpi_mutex_unlock(&g_mutex);
	if (lkpi_mutex_is_locked_by_current(&g_mutex))
		ok = 0;

	/* Mutual exclusion under a real race: two contexts each increment a
	 * non-atomic counter 200 times with a yield inside the critical section.
	 * Without exclusion the yield loses updates and the total is short. */
	g_mutex_counter = 0;
	g_mutex_thread_done = 0;
	if (kthread_create("lkpi-mutex", mutex_thread, 0) < 0) {
		ok = 0;
	} else {
		for (int i = 0; i < 200; i++) {
			lkpi_mutex_lock(&g_mutex);
			u32 v = g_mutex_counter;
			scheduler_yield();
			g_mutex_counter = v + 1;
			lkpi_mutex_unlock(&g_mutex);
		}
		for (int i = 0; i < 2000 && !g_mutex_thread_done; i++)
			scheduler_sleep_ticks(1);
		if (!g_mutex_thread_done || g_mutex_counter != 400)
			ok = 0;
	}

	lkpi_report("locks", ok, g_mutex_counter);
}

void lkpi_selftest(void)
{
	if (!bootinfo_has_flag("b1nix.test=1"))
		return;

	test_idr();
	test_completion();
	test_workqueue();
	test_scatterlist();
	test_ioremap();
	test_dma();
	test_dma_bounce();
	test_dma_bounce_sg();
	test_dma_bounce_pool();
	test_dma_bounce_sg_fallback();
	test_firmware();
	test_locks();
	console_write("M99-SMOKE: done\n");
}
