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

dma_addr_t dma_map_single(void *cpu_addr, usize size, int direction)
{
	if (!cpu_addr || size == 0)
		return 0;
	/* Translate through the page tables rather than by subtracting the
	 * direct-map base: kernel heap allocations do not live in the direct map
	 * (kmalloc hands out addresses from the kernel's own window), so the
	 * subtraction produced a "physical" address that pointed nowhere — which is
	 * exactly what a device would then have DMA'd into. The direct map stays as
	 * the fallback for addresses the walk cannot resolve. */
	u64 v = (u64)(usize)cpu_addr;
	u64 dm = vmm_direct_map_base();
	dma_addr_t handle = vmm_virt_to_phys(cpu_addr);
	if (!handle) {
		if (v < dm)
			return 0; /* untranslatable: b1nix has no bounce buffers */
		handle = v - dm;
	}
	dma_sync_single_for_device(handle, size, direction);
	return handle;
}

void dma_unmap_single(dma_addr_t handle, usize size, int direction)
{
	dma_sync_single_for_cpu(handle, size, direction);
}

void dma_sync_single_for_device(dma_addr_t handle, usize size, int direction)
{
	if (direction == DMA_FROM_DEVICE || size == 0)
		return;
	/* Push CPU writes out to memory so a non-snooping device reads them. */
	cache_flush_range((const void *)(usize)(handle + vmm_direct_map_base()),
	                  size);
}

void dma_sync_single_for_cpu(dma_addr_t handle, usize size, int direction)
{
	if (direction == DMA_TO_DEVICE || size == 0)
		return;
	/* Drop any stale cached copies of a region the device just rewrote. */
	cache_flush_range((const void *)(usize)(handle + vmm_direct_map_base()),
	                  size);
}

u32 dma_map_sg(struct sg_table *sgt, int direction)
{
	if (!sgt || !sgt->sgl || sgt->nents == 0)
		return 0;
	u64 limit = dma_addressable_limit();
	for (u32 i = 0; i < sgt->nents; i++) {
		struct scatterlist *sg = &sgt->sgl[i];
		if (sg->length == 0)
			return 0;
		if (sg->phys + sg->offset + sg->length > limit)
			return 0;
		dma_sync_single_for_device(sg->phys + sg->offset, sg->length, direction);
	}
	return sgt->nents;
}

void dma_unmap_sg(struct sg_table *sgt, int direction)
{
	if (!sgt || !sgt->sgl)
		return;
	for (u32 i = 0; i < sgt->nents; i++) {
		struct scatterlist *sg = &sgt->sgl[i];
		dma_sync_single_for_cpu(sg->phys + sg->offset, sg->length, direction);
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
