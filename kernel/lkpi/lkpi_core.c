/*
 * SPDX-License-Identifier: MIT
 *
 * M99 linuxkpi: allocator shims, ioremap, dma-mapping, sleeping mutex.
 */

#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/iommu.h>
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <lkpi/dma-mapping.h>
#include <lkpi/io.h>
#include <lkpi/lock.h>
#include <lkpi/types.h>
#include <stdio.h>
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

/*
 * Device registers: strongly uncached, PCD together with PWT.
 *
 * PCD alone is UC-, which is not the same thing. UC- defers to the MTRRs, so a
 * range the firmware or the host marked write-combining stays write-combining —
 * and then a register write may sit in a write-combining buffer instead of
 * reaching the device. A driver that writes a command register and then polls a
 * status register sees the poll time out while the command has not been issued
 * yet, and any unrelated activity that happens to flush the buffer makes the
 * same code work. Intermittent by construction.
 *
 * The pair PCD|PWT selects UC outright, which is what a register window has to
 * be. Framebuffers and apertures still get write-combining, through ioremap_wc,
 * where the reordering is the point.
 */
void *ioremap(u64 phys, usize size)
{
	return vmm_map_mmio(phys, size,
	                    VMM_WRITABLE | VMM_PCD | VMM_PWT | VMM_NO_EXECUTE);
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

void *dma_alloc_coherent(struct device *dev, usize size,
                         dma_addr_t *dma_handle, u32 gfp)
{
	(void)dev;
	(void)gfp; /* see the note in <lkpi/dma-mapping.h> */
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

void dma_free_coherent(struct device *dev, usize size, void *cpu_addr,
                       dma_addr_t dma_handle)
{
	(void)dev;
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
 * Where the blocks come from (M100a). A bounce block must be physically
 * contiguous — a device given one address cannot be handed several — and taking
 * it from the general allocator at map time makes the mapping's success depend
 * on what every other allocation did to the free lists. So a pool is reserved
 * at boot, while nothing has fragmented them yet, and slots are handed out
 * inside it. "No bounce buffer" then means "too many mappings in flight", which
 * is a condition of this subsystem with a number attached. A mask too narrow
 * for the pool still falls back to asking the allocator for frames below it —
 * the pool is the common case, not the only one.
 *
 * An sg table is bounced into ONE block when it can be, so a device with a
 * segment limit is not handed more segments than the caller built. When one
 * block cannot be had, each run is bounced on its own rather than the mapping
 * being refused: more segments is worse than one block and much better than an
 * error. Either way each entry's dma_address says where its data went, and the
 * copies are driven off that.
 */

struct dma_block {
	dma_addr_t addr;
	usize frames;
	int from_pool;
};

struct dma_bounce {
	struct dma_bounce *next;
	dma_addr_t handle;    /* lookup key: the block, or the sg's first entry */
	void *cpu;            /* single-buffer mapping: the caller's buffer */
	struct sg_table *sgt; /* sg mapping: the table whose runs were copied */
	usize size;
	int direction;
	u32 nblocks;
	struct dma_block *blocks;
};

static struct dma_bounce *g_bounce_list;
static spinlock_t g_bounce_lock = SPINLOCK_INIT;

/* ── the reserved pool ──────────────────────────────────────────── */

#define DMA_POOL_DEFAULT_KIB 4096u

static u64 g_pool_base;
static usize g_pool_frames;
static u8 *g_pool_used;        /* one byte per frame; 1 = handed out */
static usize g_pool_in_use;    /* frames currently handed out */
static usize g_pool_peak;      /* high-water mark, for the exhaustion report */
static usize g_pool_mappings;  /* mappings currently holding pool frames */
static spinlock_t g_pool_lock = SPINLOCK_INIT;

/* Fault injection (test only): refuse any pool or allocator block larger than
 * one page, which is what a fragmented system looks like from in here. It is
 * the only way to reach the per-run sg fallback deliberately — under QEMU the
 * allocator always finds a contiguous run. */
static int g_bounce_no_contig;

void dma_bounce_force_single_page(int on) { g_bounce_no_contig = on ? 1 : 0; }

int dma_bounce_pool_range(u64 *base, u64 *end)
{
	if (!g_pool_base)
		return -1;
	if (base)
		*base = g_pool_base;
	if (end)
		*end = g_pool_base + (u64)g_pool_frames * PAGE_SIZE;
	return 0;
}

void dma_bounce_pool_stats(usize *frames, usize *in_use, usize *peak,
                           usize *mappings)
{
	u64 flags;
	spin_lock_irqsave(&g_pool_lock, &flags);
	if (frames) *frames = g_pool_frames;
	if (in_use) *in_use = g_pool_in_use;
	if (peak) *peak = g_pool_peak;
	if (mappings) *mappings = g_pool_mappings;
	spin_unlock_irqrestore(&g_pool_lock, flags);
}

void dma_bounce_pool_init(void)
{
	if (g_pool_base)
		return;

	u32 kib = DMA_POOL_DEFAULT_KIB;
	char buf[32];
	if (bootinfo_get_kv("b1nix.bounce-pool", buf, sizeof(buf)) && buf[0]) {
		u32 v = 0;
		for (const char *p = buf; *p >= '0' && *p <= '9'; p++)
			v = v * 10 + (u32)(*p - '0');
		kib = v; /* 0 disables the pool: every bounce then asks the allocator */
	}
	if (kib == 0) {
		console_write("dma: bounce pool disabled by cmdline\n");
		return;
	}

	usize frames = ((usize)kib * 1024 + PAGE_SIZE - 1) / PAGE_SIZE;
	/* Below 4 GiB: the window every 32-bit-capable device can reach, and the
	 * one a narrower device is most likely to be a prefix of. */
	u64 base = pmm_alloc_frames_below(frames, 0xFFFFFFFFULL);
	if (!base) {
		console_write("dma: bounce pool reservation failed\n");
		return;
	}
	u8 *used = kmalloc(frames);
	if (!used) {
		for (usize i = 0; i < frames; i++)
			pmm_free_frame(base + (u64)i * PAGE_SIZE);
		console_write("dma: bounce pool bookkeeping allocation failed\n");
		return;
	}
	memset(used, 0, frames);

	g_pool_base = base;
	g_pool_frames = frames;
	g_pool_used = used;

	char line[128];
	snprintf(line, sizeof(line),
	         "dma: bounce pool %lu KiB at 0x%lx\n",
	         (unsigned long)((u64)frames * PAGE_SIZE / 1024),
	         (unsigned long)base);
	console_write(line);
}

/* First fit over the pool's frame map. Returns 0 when no run of `frames`
 * contiguous pages is free, or when the pool does not reach under `dma_mask`. */
static dma_addr_t pool_alloc(usize frames, u64 dma_mask)
{
	if (!g_pool_base || frames == 0)
		return 0;
	if (g_bounce_no_contig && frames > 1)
		return 0;
	u64 pool_end = g_pool_base + (u64)g_pool_frames * PAGE_SIZE - 1;
	if (pool_end > dma_mask) {
		/* Part of the pool may still be low enough; only use the prefix that
		 * the device can address. */
		if (g_pool_base > dma_mask)
			return 0;
	}

	u64 flags;
	dma_addr_t got = 0;
	spin_lock_irqsave(&g_pool_lock, &flags);
	usize run = 0;
	for (usize i = 0; i < g_pool_frames; i++) {
		u64 addr = g_pool_base + (u64)i * PAGE_SIZE;
		if (g_pool_used[i] || addr + PAGE_SIZE - 1 > dma_mask) {
			run = 0;
			continue;
		}
		if (++run == frames) {
			usize start = i + 1 - frames;
			for (usize j = 0; j < frames; j++)
				g_pool_used[start + j] = 1;
			g_pool_in_use += frames;
			if (g_pool_in_use > g_pool_peak)
				g_pool_peak = g_pool_in_use;
			got = g_pool_base + (u64)start * PAGE_SIZE;
			break;
		}
	}
	spin_unlock_irqrestore(&g_pool_lock, flags);
	return got;
}

static void pool_free(dma_addr_t addr, usize frames)
{
	u64 flags;
	spin_lock_irqsave(&g_pool_lock, &flags);
	usize start = (usize)((addr - g_pool_base) / PAGE_SIZE);
	for (usize j = 0; j < frames && start + j < g_pool_frames; j++)
		g_pool_used[start + j] = 0;
	g_pool_in_use -= frames <= g_pool_in_use ? frames : g_pool_in_use;
	spin_unlock_irqrestore(&g_pool_lock, flags);
}

/* One block of `frames` contiguous pages under `dma_mask`: the pool first, then
 * the general allocator for a mask the pool cannot satisfy. */
static int block_alloc(usize frames, u64 dma_mask, struct dma_block *out)
{
	dma_addr_t addr = pool_alloc(frames, dma_mask);
	if (addr) {
		out->addr = addr;
		out->frames = frames;
		out->from_pool = 1;
		return 0;
	}
	if (g_bounce_no_contig && frames > 1)
		return -1;
	addr = pmm_alloc_frames_below(frames, dma_mask);
	if (!addr)
		return -1;
	out->addr = addr;
	out->frames = frames;
	out->from_pool = 0;
	return 0;
}

static void block_free(const struct dma_block *b)
{
	if (b->from_pool) {
		pool_free(b->addr, b->frames);
		return;
	}
	for (usize i = 0; i < b->frames; i++)
		pmm_free_frame(b->addr + (u64)i * PAGE_SIZE);
}

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

/* How many separate blocks a bounced mapping is made of: 1 when the whole
 * table fitted in one, one per run when it did not. 0 when the handle names no
 * bounce mapping. Lets a test observe which path ran instead of inferring it
 * from addresses, which can land adjacent by chance. */
u32 dma_bounce_mapping_blocks(dma_addr_t handle)
{
	u64 flags;
	spin_lock_irqsave(&g_bounce_lock, &flags);
	struct dma_bounce *b = bounce_lookup(handle);
	u32 n = b ? b->nblocks : 0;
	spin_unlock_irqrestore(&g_bounce_lock, flags);
	return n;
}

int dma_mapping_is_bounced(dma_addr_t handle)
{
	u64 flags;
	spin_lock_irqsave(&g_bounce_lock, &flags);
	int bounced = bounce_lookup(handle) != 0;
	spin_unlock_irqrestore(&g_bounce_lock, flags);
	return bounced;
}

/* The copies are driven off each sg entry's dma_address, so one block and a
 * block per run are the same code. */
static void bounce_copy_in(const struct dma_bounce *b)
{
	if (b->direction == DMA_FROM_DEVICE)
		return;
	if (b->sgt) {
		for (u32 i = 0; i < b->sgt->nents; i++) {
			struct scatterlist *sg = &b->sgt->sgl[i];
			memcpy(bounce_kva(sg->dma_address),
			       (const void *)(usize)(sg->phys + sg->offset +
			                             vmm_direct_map_base()),
			       sg->length);
		}
		return;
	}
	memcpy(bounce_kva(b->handle), b->cpu, b->size);
}

static void bounce_copy_out(const struct dma_bounce *b)
{
	if (b->direction == DMA_TO_DEVICE)
		return;
	if (b->sgt) {
		for (u32 i = 0; i < b->sgt->nents; i++) {
			struct scatterlist *sg = &b->sgt->sgl[i];
			memcpy((void *)(usize)(sg->phys + sg->offset +
			                       vmm_direct_map_base()),
			       bounce_kva(sg->dma_address), sg->length);
		}
		return;
	}
	memcpy(b->cpu, bounce_kva(b->handle), b->size);
}

static void bounce_publish(struct dma_bounce *b)
{
	u64 flags;
	spin_lock_irqsave(&g_bounce_lock, &flags);
	b->next = g_bounce_list;
	g_bounce_list = b;
	spin_unlock_irqrestore(&g_bounce_lock, flags);

	int pooled = 0;
	for (u32 i = 0; i < b->nblocks; i++)
		pooled |= b->blocks[i].from_pool;
	if (pooled) {
		spin_lock_irqsave(&g_pool_lock, &flags);
		g_pool_mappings++;
		spin_unlock_irqrestore(&g_pool_lock, flags);
	}
}

static struct dma_bounce *bounce_record_new(u32 nblocks)
{
	struct dma_bounce *b = kmalloc(sizeof(*b));
	if (!b)
		return 0;
	b->blocks = kmalloc(sizeof(struct dma_block) * nblocks);
	if (!b->blocks) {
		kfree(b);
		return 0;
	}
	b->next = 0;
	b->handle = 0;
	b->cpu = 0;
	b->sgt = 0;
	b->size = 0;
	b->direction = 0;
	b->nblocks = nblocks;
	return b;
}

static void bounce_record_free(struct dma_bounce *b)
{
	kfree(b->blocks);
	kfree(b);
}

/* Single-buffer bounce: one block, the caller's data copied in. */
static dma_addr_t bounce_map(void *cpu_addr, usize size, int direction,
                             u64 dma_mask)
{
	if (size == 0)
		return 0;
	struct dma_bounce *b = bounce_record_new(1);
	if (!b)
		return 0;
	usize frames = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (block_alloc(frames, dma_mask, &b->blocks[0]) != 0) {
		bounce_record_free(b);
		return 0;
	}
	b->handle = b->blocks[0].addr;
	b->cpu = cpu_addr;
	b->size = size;
	b->direction = direction;
	bounce_copy_in(b);
	bounce_publish(b);
	dma_cache_for_device(b->handle, size, direction);
	return b->handle;
}

/* Whole-table bounce, falling back to a block per run. Fills in every entry's
 * dma_address. Returns the address of the first entry, or 0. */
static dma_addr_t bounce_map_sg(struct sg_table *sgt, u64 total, int direction,
                                u64 dma_mask)
{
	usize frames = ((usize)total + PAGE_SIZE - 1) / PAGE_SIZE;
	struct dma_bounce *b = bounce_record_new(1);
	if (!b)
		return 0;

	if (block_alloc(frames, dma_mask, &b->blocks[0]) == 0) {
		u64 off = 0;
		for (u32 i = 0; i < sgt->nents; i++) {
			sgt->sgl[i].dma_address = b->blocks[0].addr + off;
			off += sgt->sgl[i].length;
		}
		b->handle = b->blocks[0].addr;
		b->sgt = sgt;
		b->size = (usize)total;
		b->direction = direction;
		bounce_copy_in(b);
		bounce_publish(b);
		dma_cache_for_device(b->handle, (usize)total, direction);
		return b->handle;
	}
	bounce_record_free(b);

	/* No single block that large. Bounce each run on its own: the device gets
	 * more segments than the caller built, which is the cost of not refusing a
	 * mapping the system could still serve. */
	b = bounce_record_new(sgt->nents);
	if (!b)
		return 0;
	for (u32 i = 0; i < sgt->nents; i++) {
		usize run_frames =
		    ((usize)sgt->sgl[i].length + PAGE_SIZE - 1) / PAGE_SIZE;
		if (block_alloc(run_frames, dma_mask, &b->blocks[i]) != 0) {
			for (u32 j = 0; j < i; j++)
				block_free(&b->blocks[j]);
			bounce_record_free(b);
			return 0;
		}
		sgt->sgl[i].dma_address = b->blocks[i].addr;
	}
	b->handle = b->blocks[0].addr;
	b->sgt = sgt;
	b->size = (usize)total;
	b->direction = direction;
	bounce_copy_in(b);
	bounce_publish(b);
	for (u32 i = 0; i < sgt->nents; i++)
		dma_cache_for_device(sgt->sgl[i].dma_address, sgt->sgl[i].length,
		                     direction);
	return b->handle;
}

/* Copy back whatever the device wrote and release every block. Returns 1 if the
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

	int pooled = 0;
	for (u32 i = 0; i < b->nblocks; i++) {
		pooled |= b->blocks[i].from_pool;
		block_free(&b->blocks[i]);
	}
	if (pooled) {
		spin_lock_irqsave(&g_pool_lock, &flags);
		if (g_pool_mappings)
			g_pool_mappings--;
		spin_unlock_irqrestore(&g_pool_lock, flags);
	}
	bounce_record_free(b);
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

	/* Out of the device's reach: copy the table into memory it can address —
	 * one block if one can be had (so a device with a segment limit sees no
	 * more segments than the caller built), otherwise a block per run. Entries
	 * keep their lengths and order either way; only the addresses move. */
	if (!bounce_map_sg(sgt, total, direction, dma_mask))
		return 0;
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

/* ── device-scoped mapping (IOMMU) ──────────────────────────────── */

int dma_device_attach(struct dma_device *dev, u8 bus, u8 slot, u8 func,
                      u64 dma_mask)
{
	if (!dev)
		return -1;
	dev->bus = bus;
	dev->slot = slot;
	dev->func = func;
	dev->dma_mask = dma_mask ? dma_mask : dma_addressable_limit() - 1;
	dev->translated = 0;
	if (!iommu_active())
		return -1;
	if (iommu_attach_device(bus, slot, func) != 0)
		return -1;
	dev->translated = 1;
	return 0;
}

void dma_device_detach(struct dma_device *dev)
{
	if (!dev || !dev->translated)
		return;
	iommu_detach_device(dev->bus, dev->slot, dev->func);
	dev->translated = 0;
}

dma_addr_t dma_map_single_dev(struct dma_device *dev, void *cpu_addr,
                              usize size, int direction)
{
	if (!dev || !dev->translated)
		return dma_map_single_masked(cpu_addr, size,
		                             direction,
		                             dev ? dev->dma_mask
		                                 : dma_addressable_limit() - 1);
	if (!cpu_addr || size == 0)
		return 0;

	u64 phys = vmm_virt_to_phys(cpu_addr);
	u64 dm = vmm_direct_map_base();
	if (!phys && (u64)(usize)cpu_addr >= dm)
		phys = (u64)(usize)cpu_addr - dm;
	if (!phys)
		return 0;

	/* Map whole pages: the unit translates at page granularity, and the byte
	 * offset rides along in the address handed back. */
	u64 page = phys & ~(u64)(PAGE_SIZE - 1);
	u64 offset = phys - page;
	usize span = (usize)(offset + size);
	u64 iova = iommu_iova_alloc(span);
	if (!iova)
		return 0;
	if (iommu_map(iova, page, span, direction != DMA_TO_DEVICE) != 0) {
		iommu_iova_free(iova, span);
		return 0;
	}
	dma_cache_for_device(phys, size, direction);
	return iova + offset;
}

void dma_unmap_single_dev(struct dma_device *dev, dma_addr_t handle,
                          usize size, int direction)
{
	if (!dev || !dev->translated) {
		dma_unmap_single(handle, size, direction);
		return;
	}
	u64 iova = handle & ~(u64)(PAGE_SIZE - 1);
	usize span = (usize)((handle - iova) + size);
	u64 phys = iommu_translate(handle);
	iommu_unmap(iova, span);
	iommu_iova_free(iova, span);
	if (phys)
		dma_cache_for_cpu(phys, size, direction);
}

u32 dma_map_sg_dev(struct dma_device *dev, struct sg_table *sgt, int direction)
{
	if (!dev || !dev->translated)
		return dma_map_sg_masked(sgt, direction,
		                         dev ? dev->dma_mask
		                             : dma_addressable_limit() - 1);
	if (!sgt || !sgt->sgl || sgt->nents == 0)
		return 0;

	/* One IOVA range for the whole table: the runs are scattered in physical
	 * memory, but the device sees them contiguous. That is the thing an IOMMU
	 * can do and a bounce buffer can only imitate by copying. */
	u64 total = 0;
	for (u32 i = 0; i < sgt->nents; i++) {
		if (sgt->sgl[i].length == 0)
			return 0;
		total += sgt->sgl[i].length;
	}
	u64 base = iommu_iova_alloc((usize)total);
	if (!base)
		return 0;

	u64 off = 0;
	for (u32 i = 0; i < sgt->nents; i++) {
		struct scatterlist *sg = &sgt->sgl[i];
		u64 phys = sg->phys + sg->offset;
		if (iommu_map(base + off, phys & ~(u64)(PAGE_SIZE - 1),
		              (usize)(sg->length + (phys & (PAGE_SIZE - 1))),
		              direction != DMA_TO_DEVICE) != 0) {
			iommu_unmap(base, (usize)total);
			iommu_iova_free(base, (usize)total);
			return 0;
		}
		sg->dma_address = base + off + (phys & (PAGE_SIZE - 1));
		dma_cache_for_device(phys, sg->length, direction);
		off += sg->length;
	}
	return sgt->nents;
}

void dma_unmap_sg_dev(struct dma_device *dev, struct sg_table *sgt,
                      int direction)
{
	if (!dev || !dev->translated) {
		dma_unmap_sg(sgt, direction);
		return;
	}
	if (!sgt || !sgt->sgl || sgt->nents == 0)
		return;
	u64 total = 0;
	for (u32 i = 0; i < sgt->nents; i++) {
		dma_cache_for_cpu(sgt->sgl[i].phys + sgt->sgl[i].offset,
		                  sgt->sgl[i].length, direction);
		total += sgt->sgl[i].length;
	}
	u64 base = sgt->sgl[0].dma_address & ~(u64)(PAGE_SIZE - 1);
	iommu_unmap(base, (usize)total);
	iommu_iova_free(base, (usize)total);
	for (u32 i = 0; i < sgt->nents; i++)
		sgt->sgl[i].dma_address = 0;
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
