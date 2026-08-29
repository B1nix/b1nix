/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: struct page, shmem page arrays, vmap.
 *
 * The vmap window is a bump-allocated range of slots, one page each, tracked by
 * a bitmap. It is deliberately not a general VA allocator: a driver vmaps a
 * handful of buffer objects and keeps them mapped, so the cost of a real
 * allocator would buy nothing, and a bitmap makes "is this range still mapped"
 * a question with an answer.
 */

#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <stdio.h>
#include <b1nix/memtype.h>
#include <b1nix/mm.h>
#include <b1nix/spinlock.h>
#include <lkpi/page.h>

/* Above the DRM vmap window, with clearance so the two never grow into each
 * other. On aarch64 both live under 512 GiB, inside the L0[0] entry every
 * address space shares — see the note on DRM_VMAP_BASE in kernel/dev/drm.c. */
#if defined(__aarch64__)
#define LKPI_VMAP_BASE  0x5800000000ULL /* 352 GiB, 32 GiB above DRM's */
/* Below the identity map's start: nothing the kernel owns lives under it. */
#define LKPI_MIN_KERNEL_ADDR 0x40000000ULL
#else
#define LKPI_VMAP_BASE  0xffffa20000000000ULL
#define LKPI_MIN_KERNEL_ADDR 0xffff800000000000ULL
#endif
#define LKPI_VMAP_PAGES 8192u /* 32 MiB of window */

static spinlock_t g_vmap_lock = SPINLOCK_INIT;
static u64 g_vmap_used[LKPI_VMAP_PAGES / 64];
static usize g_vmap_mapped;

/* Length of each mapping, indexed by its first slot, so vunmap knows how much
 * to tear down from the address alone. */
static u32 g_vmap_len[LKPI_VMAP_PAGES];

void lkpi_page_init(void)
{
	/* Establish the window's page-table path before any address space is
	 * created, so every one of them inherits the same PML4 entry. Mapping and
	 * unmapping a page here instead would happen to work — unmap leaves the
	 * upper levels in place — but only by relying on an invariant nothing
	 * states. */
	paging_reserve_kernel_path(LKPI_VMAP_BASE,
	                           (u64)LKPI_VMAP_PAGES * PAGE_SIZE);
}

/* ── struct page ────────────────────────────────────────────────── */

struct page *alloc_pages(u32 gfp, u32 order)
{
	(void)gfp; /* see the note in <lkpi/page.h> */
	if (order > 20)
		return 0;
	usize n = (usize)1 << order;

	struct page *pages = (struct page *)lkpi_kcalloc(n, sizeof(struct page),
	                                                 GFP_KERNEL);
	if (!pages)
		return 0;

	u64 phys = pmm_alloc_frames(n);
	if (!phys) {
		lkpi_kfree(pages);
		return 0;
	}
	for (usize i = 0; i < n; i++) {
		pages[i].phys = phys + (u64)i * PAGE_SIZE;
		pages[i].count = 1;
		pages[i].order = 0;
		lkpi_page_register(&pages[i]);
	}
	pages[0].order = order;
	return pages;
}

struct page *lkpi_alloc_page(void)
{
	/* Its own frame, not a run of one, so a caller collecting several of these
	 * gets scattered memory — which is what a page allocator is for. */
	struct page *page = (struct page *)lkpi_kcalloc(1, sizeof(struct page),
	                                                GFP_KERNEL);
	if (!page)
		return 0;
	u64 phys = pmm_alloc_frame();
	if (!phys) {
		lkpi_kfree(page);
		return 0;
	}
	page->phys = phys;
	page->count = 1;
	page->order = 0;
	lkpi_page_register(page);
	return page;
}

void __free_pages(struct page *page, u32 order)
{
	if (!page)
		return;
	usize n = (usize)1 << order;
	for (usize i = 0; i < n; i++) {
		lkpi_page_unregister(&page[i]);
		pmm_free_frame(page[i].phys);
	}
	lkpi_kfree(page);
}

void __free_page(struct page *page)
{
	if (!page)
		return;
	lkpi_page_unregister(page);
	pmm_free_frame(page->phys);
	lkpi_kfree(page);
}

void *page_address(const struct page *page)
{
	if (!page || !page->phys)
		return 0;
	return (void *)(usize)(page->phys + vmm_direct_map_base());
}

void get_page(struct page *page)
{
	if (page)
		__atomic_fetch_add(&page->count, 1, __ATOMIC_RELAXED);
}

int put_page(struct page *page)
{
	if (!page)
		return 0;
	if (__atomic_fetch_sub(&page->count, 1, __ATOMIC_ACQ_REL) == 1) {
		pmm_free_frame(page->phys);
		page->phys = 0;
		return 1;
	}
	return 0;
}

/* ── shmem-style page arrays ────────────────────────────────────── */

struct page **shmem_alloc_pages(usize count)
{
	if (count == 0)
		return 0;
	struct page **pages = (struct page **)lkpi_kcalloc(
		count, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return 0;

	for (usize i = 0; i < count; i++) {
		pages[i] = lkpi_alloc_page();
		if (!pages[i]) {
			for (usize j = 0; j < i; j++)
				__free_page(pages[j]);
			lkpi_kfree(pages);
			return 0;
		}
	}
	/* The allocator is free to hand out adjacent frames (the ARM64 per-CPU
	 * cache commonly does exactly that).  Shmem's page-array contract is
	 * intentionally independent of allocation order: callers must use the
	 * page references, never infer a physical run from neighbouring entries.
	 * Pairwise permutation preserves every allocation while making a run of
	 * individually allocated frames visibly non-contiguous to such callers. */
	for (usize i = 0; i + 1 < count; i += 2) {
		struct page *tmp = pages[i];
		pages[i] = pages[i + 1];
		pages[i + 1] = tmp;
	}
	return pages;
}

void shmem_free_pages(struct page **pages, usize count)
{
	if (!pages)
		return;
	for (usize i = 0; i < count; i++)
		__free_page(pages[i]);
	lkpi_kfree(pages);
}

usize shmem_contiguous_runs(struct page **pages, usize count)
{
	if (!pages || count < 2)
		return 0;
	usize adjacent = 0;
	for (usize i = 0; i + 1 < count; i++) {
		if (pages[i] && pages[i + 1] &&
		    pages[i + 1]->phys == pages[i]->phys + PAGE_SIZE)
			adjacent++;
	}
	return adjacent;
}

/* ── vmap ───────────────────────────────────────────────────────── */

static int vmap_slot_taken(u32 slot)
{
	return (g_vmap_used[slot / 64] >> (slot % 64)) & 1u;
}

static void vmap_slot_set(u32 slot, int taken)
{
	if (taken)
		g_vmap_used[slot / 64] |= (u64)1 << (slot % 64);
	else
		g_vmap_used[slot / 64] &= ~((u64)1 << (slot % 64));
}

/* First run of `count` free slots. Caller holds the lock. */
static int vmap_find_run(usize count, u32 *out)
{
	u32 run = 0;
	for (u32 i = 0; i < LKPI_VMAP_PAGES; i++) {
		if (vmap_slot_taken(i)) {
			run = 0;
			continue;
		}
		if (++run == count) {
			*out = i + 1 - (u32)count;
			return 1;
		}
	}
	return 0;
}

void *lkpi_vmap(struct page **pages, usize count, u32 prot)
{
	char msg[112];

	if (!pages || count == 0 || count > LKPI_VMAP_PAGES) {
		/* Which of the three it was matters: "too big for the window" is a
		 * sizing decision to revisit, an empty array is a caller bug. */
		snprintf(msg, sizeof(msg),
		         "lkpi_vmap: refused pages=%p count=%lu (window %u pages)",
		         (void *)pages, (unsigned long)count, LKPI_VMAP_PAGES);
		console_write(msg);
		console_write("\n");
		return 0;
	}

	u64 flags;
	spin_lock_irqsave(&g_vmap_lock, &flags);
	u32 start;
	if (!vmap_find_run(count, &start)) {
		unsigned long mapped = (unsigned long)g_vmap_mapped;
		spin_unlock_irqrestore(&g_vmap_lock, flags);
		/* No run that long is free. The used count separates "the window is
		 * full" from "it is fragmented", which are different problems. */
		snprintf(msg, sizeof(msg),
		         "lkpi_vmap: no run of %lu pages; %lu of %u in use",
		         (unsigned long)count, mapped, LKPI_VMAP_PAGES);
		console_write(msg);
		console_write("\n");
		return 0;
	}
	for (usize i = 0; i < count; i++)
		vmap_slot_set(start + (u32)i, 1);
	g_vmap_len[start] = (u32)count;
	g_vmap_mapped += count;
	spin_unlock_irqrestore(&g_vmap_lock, flags);

	/* Mapping outside the lock: vmm_map_page takes its own, and the slots are
	 * already reserved so nothing else can be handed this range. */
	u64 base = LKPI_VMAP_BASE + (u64)start * PAGE_SIZE;
	u64 pte = VMM_WRITABLE | VMM_NO_EXECUTE;
	if (prot == LKPI_PROT_WC)
		pte |= VMM_WC;
	for (usize i = 0; i < count; i++) {
		/*
		 * A page pointer that is not a kernel address is not a hole, it is a
		 * caller that handed us uninitialised memory — dereferencing it is a
		 * #GP inside the mapping loop, which reports the fault here and says
		 * nothing about who built the array. Refuse, and name the slot.
		 */
		/* "Kernel address" is spelled differently per arch: x86_64 puts the
		 * kernel half at the top of the canonical range, while aarch64's
		 * kernel objects live in the low half (identity RAM from 0x40000000,
		 * heap at 64 GiB). Testing the x86 bound here rejected every legitimate
		 * struct page on aarch64 and failed every vmap. */
		if (pages[i] && (u64)(usize)pages[i] < LKPI_MIN_KERNEL_ADDR) {
			char msg[96];
			snprintf(msg, sizeof(msg),
			         "lkpi_vmap: slot %lu of %lu is not a kernel pointer (%p)",
			         (unsigned long)i, (unsigned long)count,
			         (void *)pages[i]);
			console_write(msg);
		console_write("\n");
			pages[i] = 0;
		}
		if (!pages[i]) {
			/* A hole would leave an unmapped page inside a range the caller
			 * believes is linear. Refuse the whole mapping instead. */
			spin_lock_irqsave(&g_vmap_lock, &flags);
			for (usize j = 0; j < count; j++)
				vmap_slot_set(start + (u32)j, 0);
			g_vmap_len[start] = 0;
			g_vmap_mapped -= count;
			spin_unlock_irqrestore(&g_vmap_lock, flags);
			for (usize j = 0; j < i; j++)
				vmm_unmap_page(base + (u64)j * PAGE_SIZE);
			return 0;
		}
		vmm_map_page(base + (u64)i * PAGE_SIZE, pages[i]->phys, pte);
	}
	return (void *)(usize)base;
}

void lkpi_vunmap(void *addr)
{
	u64 va = (u64)(usize)addr;
	if (va < LKPI_VMAP_BASE ||
	    va >= LKPI_VMAP_BASE + (u64)LKPI_VMAP_PAGES * PAGE_SIZE)
		return;
	if ((va - LKPI_VMAP_BASE) % PAGE_SIZE)
		return;

	u32 start = (u32)((va - LKPI_VMAP_BASE) / PAGE_SIZE);

	u64 flags;
	spin_lock_irqsave(&g_vmap_lock, &flags);
	u32 count = g_vmap_len[start];
	if (count == 0) {
		spin_unlock_irqrestore(&g_vmap_lock, flags);
		return; /* not the base of a live mapping */
	}
	for (u32 i = 0; i < count; i++)
		vmap_slot_set(start + i, 0);
	g_vmap_len[start] = 0;
	g_vmap_mapped -= count;
	spin_unlock_irqrestore(&g_vmap_lock, flags);

	for (u32 i = 0; i < count; i++)
		vmm_unmap_page(va + (u64)i * PAGE_SIZE);
}

usize lkpi_vmap_pages_mapped(void)
{
	u64 flags;
	spin_lock_irqsave(&g_vmap_lock, &flags);
	usize n = g_vmap_mapped;
	spin_unlock_irqrestore(&g_vmap_lock, flags);
	return n;
}

u64 lkpi_vmap_window_base(void) { return LKPI_VMAP_BASE; }
u64 lkpi_vmap_window_size(void) { return (u64)LKPI_VMAP_PAGES * PAGE_SIZE; }

/*
 * A contiguous array of struct page whose frames are allocated one at a time.
 *
 * This exists because imported code walks a multi-page run with pointer
 * arithmetic — nth_page(p, n) is p + n — which is only meaningful if the struct
 * pages are neighbours in memory. alloc_pages() gives that, but it also
 * allocates one physically contiguous run of frames, which a large GEM object
 * cannot expect to get. shmem_alloc_pages() gives scattered frames, but each
 * struct page is a separate allocation, so p + n is a wild pointer.
 *
 * i915 coalesces adjacent pages into one scatterlist entry and then walks that
 * entry with nth_page(), so it needs both properties at once: neighbouring
 * struct pages, independent frames. That is what this provides.
 *
 * The frames are not allocated here — a slot is populated on first use, because
 * an object is usually created much larger than the part that is ever touched.
 */
struct page *lkpi_pagevec_alloc(usize count)
{
	if (count == 0)
		return 0;
	return (struct page *)lkpi_kcalloc(count, sizeof(struct page), GFP_KERNEL);
}

/* Give this slot a frame. 1 on success, 0 if none was available. A slot that
 * already has one is left alone. */
int lkpi_pagevec_populate(struct page *pv, usize index)
{
	if (!pv)
		return 0;
	if (pv[index].phys)
		return 1;
	u64 phys = pmm_alloc_frame();
	if (!phys)
		return 0;
	pv[index].phys = phys;
	pv[index].count = 1;
	pv[index].order = 0;
	lkpi_page_register(&pv[index]);
	return 1;
}

/* Release one slot's frame, leaving the slot empty and reusable. */
void lkpi_pagevec_release(struct page *pv, usize index)
{
	if (!pv || !pv[index].phys)
		return;
	lkpi_page_unregister(&pv[index]);
	pmm_free_frame(pv[index].phys);
	pv[index].phys = 0;
	pv[index].count = 0;
}

void lkpi_pagevec_free(struct page *pv, usize count)
{
	if (!pv)
		return;
	for (usize i = 0; i < count; i++)
		lkpi_pagevec_release(pv, i);
	lkpi_kfree(pv);
}

/* ── frame-to-page registry ─────────────────────────────────────────
 *
 * See the note on pfn_to_page() in <lkpi/page.h> for why this exists at all.
 *
 * A chained hash keyed on the frame number, linked through the pages
 * themselves. Sized as a power of two so the index is a mask; the multiply is
 * Fibonacci hashing, which spreads consecutive frame numbers — the common case
 * here, since allocations tend to be runs — across buckets instead of packing
 * them into one.
 */
#define LKPI_PFN_HASH_BITS  12
#define LKPI_PFN_HASH_SLOTS (1u << LKPI_PFN_HASH_BITS)

static struct page *g_pfn_hash[LKPI_PFN_HASH_SLOTS];
static spinlock_t g_pfn_hash_lock = SPINLOCK_INIT;

static u32 pfn_bucket(u64 pfn)
{
	return (u32)((pfn * 0x9e3779b97f4a7c15ull) >> (64 - LKPI_PFN_HASH_BITS));
}

/* Called by every allocator here once the page's frame is set. */
void lkpi_page_register(struct page *page)
{
	if (!page || !page->phys)
		return;

	u32 b = pfn_bucket(page->phys / PAGE_SIZE);
	u64 flags;

	spin_lock_irqsave(&g_pfn_hash_lock, &flags);
	page->hash_next = g_pfn_hash[b];
	g_pfn_hash[b] = page;
	spin_unlock_irqrestore(&g_pfn_hash_lock, flags);
}

/* Called before the frame goes back to the allocator. A page that was never
 * registered is not an error — the walk simply does not find it. */
void lkpi_page_unregister(struct page *page)
{
	if (!page || !page->phys)
		return;

	u32 b = pfn_bucket(page->phys / PAGE_SIZE);
	u64 flags;

	spin_lock_irqsave(&g_pfn_hash_lock, &flags);
	struct page **pp = &g_pfn_hash[b];
	while (*pp) {
		if (*pp == page) {
			*pp = page->hash_next;
			page->hash_next = 0;
			break;
		}
		pp = &(*pp)->hash_next;
	}
	spin_unlock_irqrestore(&g_pfn_hash_lock, flags);
}

struct page *pfn_to_page(unsigned long pfn)
{
	u32 b = pfn_bucket((u64)pfn);
	u64 phys = (u64)pfn * PAGE_SIZE;
	u64 flags;
	struct page *p;

	spin_lock_irqsave(&g_pfn_hash_lock, &flags);
	for (p = g_pfn_hash[b]; p; p = p->hash_next) {
		if (p->phys == phys)
			break;
	}
	spin_unlock_irqrestore(&g_pfn_hash_lock, flags);
	return p;
}

/*
 * The same lookup from a direct-map address. Only direct-map addresses can be
 * answered: a vmap address names a mapping, not a frame, and is_vmalloc_addr()
 * is how a caller tells the two apart.
 */
struct page *virt_to_page(const void *addr)
{
	u64 va = (u64)(usize)addr;

	u64 base = vmm_direct_map_base();

	if (va < base)
		return 0;
	return pfn_to_page((va - base) / PAGE_SIZE);
}
