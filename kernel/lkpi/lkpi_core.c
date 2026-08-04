/*
 * SPDX-License-Identifier: MIT
 *
 * M99 linuxkpi: allocator shims, ioremap, dma-mapping, sleeping mutex.
 */

#include <b1nix/console.h>
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <lkpi/dma-mapping.h>
#include <lkpi/io.h>
#include <lkpi/lock.h>
#include <lkpi/types.h>
#include <string.h>

/* ── allocation ─────────────────────────────────────────────────── */

void *lkpi_kmalloc(usize size, gfp_t flags)
{
	if (size == 0)
		return 0;
	void *p = (flags & __GFP_ZERO) ? kzalloc(size) : kmalloc(size);
	return p;
}

void *lkpi_kcalloc(usize n, usize size, gfp_t flags)
{
	(void)flags;
	if (n == 0 || size == 0)
		return 0;
	/* Overflow check: a driver computing n from a userspace-supplied count
	 * must not be able to wrap this into a small allocation. */
	if (n > (usize)-1 / size)
		return 0;
	return kzalloc(n * size);
}

void lkpi_kfree(void *ptr)
{
	if (ptr)
		kfree(ptr);
}

/* ── ioremap ────────────────────────────────────────────────────── */

void *ioremap(u64 phys, usize size)
{
	return vmm_map_mmio(phys, size, VMM_WRITABLE | VMM_PCD | VMM_NO_EXECUTE);
}

void *ioremap_wc(u64 phys, usize size)
{
	return vmm_map_mmio(phys, size, VMM_WRITABLE | VMM_NO_EXECUTE | VMM_WC);
}

void *ioremap_wb(u64 phys, usize size)
{
	return vmm_map_mmio(phys, size, VMM_WRITABLE | VMM_NO_EXECUTE);
}

void iounmap(void *addr)
{
	/* The MMIO virtual window is a bump allocator that is never reclaimed (see
	 * vmm_map_mmio); leaving the mapping in place is correct and costs one
	 * page-table entry per device BAR for the life of the kernel. Unmapping
	 * here without a VA allocator would create a hole nothing can reuse and
	 * risk tearing down a mapping another driver still holds. */
	(void)addr;
}

/* ── dma-mapping ────────────────────────────────────────────────── */

u64 dma_addressable_limit(void)
{
	return DIRECT_MAP_SIZE;
}

void *dma_alloc_coherent(usize size, dma_addr_t *dma_handle)
{
	if (size == 0)
		return 0;
	usize frames = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 phys = pmm_alloc_frames(frames);
	if (!phys)
		return 0;
	void *cpu = (void *)(usize)(phys + vmm_direct_map_base());
	memset(cpu, 0, frames * PAGE_SIZE);
	if (dma_handle)
		*dma_handle = phys;
	return cpu;
}

void dma_free_coherent(usize size, void *cpu_addr, dma_addr_t dma_handle)
{
	if (!cpu_addr || size == 0)
		return;
	usize frames = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	for (usize i = 0; i < frames; i++)
		pmm_free_frame(dma_handle + (u64)i * PAGE_SIZE);
}

/* Cache maintenance without the bounce bookkeeping; defined with the sync
 * calls below, declared here because the bounce path is its first caller. */
static void dma_cache_for_device(dma_addr_t handle, usize size, int direction);
static void dma_cache_for_cpu(dma_addr_t handle, usize size, int direction);

/* ── bounce buffers ─────────────────────────────────────────────────
 *
 * A mapping that had to be bounced is remembered so that unmap and the sync
 * calls, which are handed only the device address, can find the caller's memory
 * again and copy in the right direction.
 *
 * Two shapes are recorded. A single-buffer mapping remembers the caller's
 * pointer. An sg mapping remembers the table: the whole table is bounced into
 * ONE block below the mask and each entry's dma_address points into it, so a
 * device that cannot reach the caller's pages sees one contiguous region rather
 * than one allocation per run — fewer allocations, and a device with a
 * segment-count limit is not handed more segments than it had before.
 *
 * The records are allocated as they are needed instead of living in a fixed
 * array: the earlier 16-slot table meant the seventeenth concurrent mapping
 * failed for no reason the driver could act on.
 */
struct dma_bounce {
	struct dma_bounce *next;
	dma_addr_t handle; /* device address of the bounce block; the lookup key */
	void *cpu;         /* single-buffer mapping: the caller's buffer */
	struct sg_table *sgt; /* sg mapping: the table whose runs were copied */
	usize size;        /* bytes bounced */
	usize frames;
	int direction;
};

static struct dma_bounce *g_bounce_list;
static spinlock_t g_bounce_lock = SPINLOCK_INIT;

/* Caller holds g_bounce_lock. */
static struct dma_bounce *bounce_lookup(dma_addr_t handle)
{
	if (!handle)
		return 0;
	for (struct dma_bounce *b = g_bounce_list; b; b = b->next)
		if (b->handle == handle)
			return b;
	return 0;
}

static void *bounce_kva(dma_addr_t handle)
{
	return (void *)(usize)(handle + vmm_direct_map_base());
}

int dma_mapping_is_bounced(dma_addr_t handle)
{
	u64 flags;
	spin_lock_irqsave(&g_bounce_lock, &flags);
	int bounced = bounce_lookup(handle) != 0;
	spin_unlock_irqrestore(&g_bounce_lock, flags);
	return bounced;
}

/* Copy the caller's data into the bounce block (device is about to read it). */
static void bounce_copy_in(const struct dma_bounce *b)
{
	if (b->direction == DMA_FROM_DEVICE)
		return;
	if (b->sgt) {
		u64 off = 0;
		for (u32 i = 0; i < b->sgt->nents; i++) {
			struct scatterlist *sg = &b->sgt->sgl[i];
			memcpy((u8 *)bounce_kva(b->handle) + off,
			       (const void *)(usize)(sg->phys + sg->offset +
			                             vmm_direct_map_base()),
			       sg->length);
			off += sg->length;
		}
		return;
	}
	memcpy(bounce_kva(b->handle), b->cpu, b->size);
}

/* Copy back whatever the device wrote. */
static void bounce_copy_out(const struct dma_bounce *b)
{
	if (b->direction == DMA_TO_DEVICE)
		return;
	if (b->sgt) {
		u64 off = 0;
		for (u32 i = 0; i < b->sgt->nents; i++) {
			struct scatterlist *sg = &b->sgt->sgl[i];
			memcpy((void *)(usize)(sg->phys + sg->offset +
			                       vmm_direct_map_base()),
			       (const u8 *)bounce_kva(b->handle) + off, sg->length);
			off += sg->length;
		}
		return;
	}
	memcpy(b->cpu, bounce_kva(b->handle), b->size);
}

/* Allocate a block of `size` bytes below `dma_mask`, record it and copy the
 * caller's data in. Exactly one of cpu_addr / sgt describes the source.
 * Returns the device address, or 0. */
static dma_addr_t bounce_map_common(void *cpu_addr, struct sg_table *sgt,
                                    usize size, int direction, u64 dma_mask)
{
	if (size == 0)
		return 0;
	struct dma_bounce *b = kmalloc(sizeof(*b));
	if (!b)
		return 0;

	usize frames = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	u64 phys = pmm_alloc_frames_below(frames, dma_mask);
	if (!phys) {
		kfree(b);
		return 0;
	}

	b->next = 0;
	b->handle = phys;
	b->cpu = cpu_addr;
	b->sgt = sgt;
	b->size = size;
	b->frames = frames;
	b->direction = direction;
	bounce_copy_in(b);

	u64 flags;
	spin_lock_irqsave(&g_bounce_lock, &flags);
	b->next = g_bounce_list;
	g_bounce_list = b;
	spin_unlock_irqrestore(&g_bounce_lock, flags);

	dma_cache_for_device(phys, size, direction);
	return phys;
}

static dma_addr_t bounce_map(void *cpu_addr, usize size, int direction,
                             u64 dma_mask)
{
	return bounce_map_common(cpu_addr, 0, size, direction, dma_mask);
}

/* Copy back whatever the device wrote and release the block. Returns 1 if the
 * handle named a bounce mapping. */
static int bounce_unmap(dma_addr_t handle, int direction)
{
	u64 flags;

	spin_lock_irqsave(&g_bounce_lock, &flags);
	struct dma_bounce *b = 0, **link = &g_bounce_list;
	for (struct dma_bounce *it = g_bounce_list; it; link = &it->next, it = it->next) {
		if (it->handle == handle) {
			b = it;
			*link = it->next;
			break;
		}
	}
	spin_unlock_irqrestore(&g_bounce_lock, flags);
	if (!b)
		return 0;

	/* The direction recorded at map time is authoritative: an unmap that names
	 * a different one is a driver bug, and honouring it here would silently
	 * skip a copy-out the caller is waiting for. */
	(void)direction;
	bounce_copy_out(b);
	for (usize i = 0; i < b->frames; i++)
		pmm_free_frame(b->handle + (u64)i * PAGE_SIZE);
	kfree(b);
	return 1;
}

dma_addr_t dma_map_single_masked(void *cpu_addr, usize size, int direction,
                                 u64 dma_mask)
{
	if (!cpu_addr || size == 0)
		return 0;
	u64 v = (u64)(usize)cpu_addr;
	u64 dm = vmm_direct_map_base();
	dma_addr_t handle = vmm_virt_to_phys(cpu_addr);
	if (!handle && v >= dm)
		handle = v - dm;

	/* Reachable as it stands: the ordinary mapping, no copy. A buffer that
	 * spans more than one page can only be handed over directly when it is
	 * physically contiguous, which for a kmalloc/direct-map buffer it is; the
	 * scatter case belongs to dma_map_sg_masked. */
	if (handle && handle + size - 1 <= dma_mask) {
		dma_cache_for_device(handle, size, direction);
		return handle;
	}

	/* Either untranslatable or outside the device's window — copy through
	 * memory the device can actually reach. */
	return bounce_map(cpu_addr, size, direction, dma_mask);
}

dma_addr_t dma_map_single(void *cpu_addr, usize size, int direction)
{
	if (!cpu_addr || size == 0)
		return 0;
	/* No mask: the device can reach everything this kernel can allocate, so
	 * this is the plain translate-and-flush path. It still goes through the
	 * masked entry point, which translates through the page tables rather than
	 * by subtracting the direct-map base — kernel heap allocations do not live
	 * in the direct map, and the subtraction produced a "physical" address that
	 * pointed nowhere, which is exactly what a device would then have DMA'd
	 * into. An address that cannot be translated at all now bounces instead of
	 * being refused. */
	return dma_map_single_masked(cpu_addr, size, direction,
	                             dma_addressable_limit() - 1);
}

void dma_unmap_single(dma_addr_t handle, usize size, int direction)
{
	if (bounce_unmap(handle, direction))
		return;
	dma_sync_single_for_cpu(handle, size, direction);
}

/* Cache maintenance only — no bounce bookkeeping. Used by the bounce path
 * itself, which has already done the copy and must not recurse into it. */
static void dma_cache_for_device(dma_addr_t handle, usize size, int direction)
{
	if (direction == DMA_FROM_DEVICE || size == 0)
		return;
	/* Push CPU writes out to memory so a non-snooping device reads them. */
	cache_flush_range((const void *)(usize)(handle + vmm_direct_map_base()),
	                  size);
}

static void dma_cache_for_cpu(dma_addr_t handle, usize size, int direction)
{
	if (direction == DMA_TO_DEVICE || size == 0)
		return;
	/* Drop any stale cached copies of a region the device just rewrote. */
	cache_flush_range((const void *)(usize)(handle + vmm_direct_map_base()),
	                  size);
}

void dma_sync_single_for_device(dma_addr_t handle, usize size, int direction)
{
	/* On a bounced mapping the device reads the bounce, not the caller's
	 * buffer, so handing the buffer back to the device means refreshing the
	 * copy — this is the sync a driver calls after touching the data again. */
	u64 flags;
	spin_lock_irqsave(&g_bounce_lock, &flags);
	struct dma_bounce *slot = bounce_lookup(handle);
	struct dma_bounce copy;
	int bounced = slot != 0;
	if (bounced)
		copy = *slot;
	spin_unlock_irqrestore(&g_bounce_lock, flags);
	if (bounced)
		bounce_copy_in(&copy);
	dma_cache_for_device(handle, size, direction);
}

void dma_sync_single_for_cpu(dma_addr_t handle, usize size, int direction)
{
	u64 flags;
	spin_lock_irqsave(&g_bounce_lock, &flags);
	struct dma_bounce *slot = bounce_lookup(handle);
	struct dma_bounce copy;
	int bounced = slot != 0;
	if (bounced)
		copy = *slot;
	spin_unlock_irqrestore(&g_bounce_lock, flags);
	dma_cache_for_cpu(handle, size, direction);
	/* ...and only then hand what the device wrote back to the caller. */
	if (bounced)
		bounce_copy_out(&copy);
}

u32 dma_map_sg_masked(struct sg_table *sgt, int direction, u64 dma_mask)
{
	if (!sgt || !sgt->sgl || sgt->nents == 0)
		return 0;

	/* One pass to decide, so the table is either mapped in place or bounced as
	 * a whole — never half of each, which would leave the device with a
	 * scatter list whose entries mean two different things. */
	u64 total = 0;
	int needs_bounce = 0;
	for (u32 i = 0; i < sgt->nents; i++) {
		struct scatterlist *sg = &sgt->sgl[i];
		if (sg->length == 0)
			return 0;
		u64 addr = sg->phys + sg->offset;
		if (addr + sg->length - 1 > dma_mask)
			needs_bounce = 1;
		total += sg->length;
	}

	if (!needs_bounce) {
		for (u32 i = 0; i < sgt->nents; i++) {
			struct scatterlist *sg = &sgt->sgl[i];
			sg->dma_address = sg->phys + sg->offset;
			dma_cache_for_device(sg->dma_address, sg->length, direction);
		}
		return sgt->nents;
	}

	/* Out of the device's reach: copy the whole table into one block below the
	 * mask. Entries keep their lengths and order, so the caller's view of the
	 * buffer is unchanged; only the addresses the device is given move, and
	 * they now describe one contiguous region. */
	dma_addr_t base = bounce_map_common(0, sgt, (usize)total, direction, dma_mask);
	if (!base)
		return 0;
	u64 off = 0;
	for (u32 i = 0; i < sgt->nents; i++) {
		struct scatterlist *sg = &sgt->sgl[i];
		sg->dma_address = base + off;
		off += sg->length;
	}
	return sgt->nents;
}

u32 dma_map_sg(struct sg_table *sgt, int direction)
{
	return dma_map_sg_masked(sgt, direction, dma_addressable_limit() - 1);
}

void dma_unmap_sg(struct sg_table *sgt, int direction)
{
	if (!sgt || !sgt->sgl || sgt->nents == 0)
		return;
	/* A bounced table has one record, keyed on the first entry's address (the
	 * base of the block); releasing it copies every run back. */
	if (bounce_unmap(sgt->sgl[0].dma_address, direction)) {
		for (u32 i = 0; i < sgt->nents; i++)
			sgt->sgl[i].dma_address = 0;
		return;
	}
	for (u32 i = 0; i < sgt->nents; i++) {
		struct scatterlist *sg = &sgt->sgl[i];
		dma_addr_t addr = sg->dma_address ? sg->dma_address
		                                  : sg->phys + sg->offset;
		dma_cache_for_cpu(addr, sg->length, direction);
		sg->dma_address = 0;
	}
}

/* ── sleeping mutex ─────────────────────────────────────────────── */

void lkpi_mutex_init(struct lkpi_mutex *m)
{
	if (!m)
		return;
	m->locked = 0;
	m->owner = 0;
	m->guard = SPINLOCK_INIT;
}

static usize lkpi_current_id(void)
{
	struct task *t = current_task;
	return t ? t->id + 1 : 0;
}

int lkpi_mutex_trylock(struct lkpi_mutex *m)
{
	if (!m)
		return 0;
	u64 flags;
	int got = 0;
	spin_lock_irqsave(&m->guard, &flags);
	if (!m->locked) {
		m->locked = 1;
		m->owner = lkpi_current_id();
		got = 1;
	}
	spin_unlock_irqrestore(&m->guard, flags);
	return got;
}

void lkpi_mutex_lock(struct lkpi_mutex *m)
{
	if (!m)
		return;
	for (;;) {
		if (lkpi_mutex_trylock(m))
			return;
		/* Two-phase wait: publish on the channel first, re-test under the
		 * guard, and only park if the lock is still held. A release between
		 * the test and the park therefore cannot be lost. */
		if (!scheduler_can_block()) {
			/* Early boot / interrupt context: spin instead of parking. A
			 * driver taking a mutex there is a bug, but hanging the machine
			 * is a worse way to report it than making progress. */
			__asm__ volatile("pause");
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare(m);
		u64 flags;
		spin_lock_irqsave(&m->guard, &flags);
		int still_held = m->locked;
		spin_unlock_irqrestore(&m->guard, flags);
		if (still_held)
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

void lkpi_mutex_unlock(struct lkpi_mutex *m)
{
	if (!m)
		return;
	u64 flags;
	spin_lock_irqsave(&m->guard, &flags);
	m->locked = 0;
	m->owner = 0;
	spin_unlock_irqrestore(&m->guard, flags);
	scheduler_wake_all(m);
}

int lkpi_mutex_is_locked_by_current(struct lkpi_mutex *m)
{
	if (!m)
		return 0;
	return m->locked && m->owner == lkpi_current_id();
}
