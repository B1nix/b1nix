/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_PAGE_H
#define LKPI_PAGE_H

#include <lkpi/types.h>
/* struct page carries a list head, so the list type has to be complete. */
#include <linux/list.h>

/* The page size, defined on this side of the boundary so <linux/mm.h> does not
 * have to reach into <b1nix/mm.h> for it. unsigned long, as on Linux: masks
 * built from it must be as wide as the addresses they are applied to. */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096UL
#endif

/*
 * struct page, shmem-backed page arrays, and vmap.
 *
 * A DRM driver does not think in "allocate n contiguous bytes". It thinks in
 * pages, because a buffer object is backed by whatever physical pages the
 * allocator had, handed to the GPU through an sg table and to userspace one page
 * at a time. Only the CPU ever wants it linear, and that is what vmap is for.
 *
 * b1nix has no global mem_map, and building one would mean a struct per physical
 * frame in the machine to serve the handful a driver actually holds. So a
 * `struct page` here is allocated alongside the frame it describes and freed
 * with it. The consequence is stated plainly because it is a real difference
 * from Linux: there is no way to go from an arbitrary physical address back to a
 * struct page. Everything that hands out pages here also hands out the array.
 *
 * Deliberately discontiguous. shmem_alloc_pages takes its frames one at a time
 * rather than as a run, so a driver that quietly assumes page[i+1] follows
 * page[i] in physical memory breaks here immediately instead of on hardware
 * whose IOMMU is off. The self-test asserts the discontiguity rather than hoping
 * for it.
 *
 * vmap maps a page array into one linear kernel range, out of a reserved window
 * whose page-table path is established at init — before any address space
 * exists, so every address space inherits it. Nothing here sleeps.
 */

/* Declared before struct page, which points at one. */
struct address_space;

struct page {
	u64 phys;            /* physical address of the frame */
	volatile i32 count;  /* references; the frame is freed when it hits 0 */
	u32 order;           /* set on the head page of an alloc_pages run */
	/*
	 * Chain in the frame-to-page registry. Threaded through the page itself
	 * rather than through separately allocated nodes: there is one of these
	 * per frame already, and a registry that allocated on every page
	 * allocation would be a second allocator inside the first.
	 */
	struct page *hash_next;
	/* A word the owner may use for its own bookkeeping. Upstream overloads
	 * it heavily; here nothing but the owner reads it. */
	unsigned long private;
	/* Linkage for whatever list currently owns the page. Upstream's reclaim
	 * uses it for the LRU; b1nix has no page LRU, so it belongs entirely to
	 * the allocator or driver holding the page. */
	struct list_head lru;

	/*
	 * The page-cache half.
	 *
	 * A page that belongs to a file carries where it belongs: which mapping,
	 * and which index within it. Both are NULL/zero for the anonymous pages a
	 * driver allocates, and that is how the two kinds are told apart — a page
	 * with a mapping may not simply be freed, because the mapping's index
	 * still points at it.
	 *
	 * `flags` is the state: up-to-date, dirty, locked, under writeback. The
	 * bit numbers are in <linux/mm.h> with the accessors, because it is the
	 * accessors that give them meaning.
	 */
	struct address_space *mapping;
	unsigned long index;
	volatile unsigned long flags;
};

/*
 * The page-state bits.
 *
 * Numbered here rather than in <linux/mm.h> because the field they describe is
 * here, and because the b1nix-side implementation of the page cache
 * (kernel/lkpi/filemap.c) must reach them without including a linux header —
 * that file also includes <b1nix/sched.h>, and the two sides define
 * `spinlock_t` differently.
 *
 * Each bit is a promise:
 *   Locked     somebody owns the page; wait before touching its contents.
 *   Uptodate   the contents match what is on disk. A reader may use the bytes.
 *   Dirty      they do not, and the DISK is the stale one.
 *   Writeback  an I/O is in flight writing it out.
 *   Private    page->private holds something the owner put there.
 *
 * Uptodate and Dirty are independent: a page just written is both, a page just
 * read is Uptodate and clean, and a page being filled is neither.
 */
enum lkpi_pageflags {
	PG_locked,
	PG_referenced,
	PG_uptodate,
	PG_dirty,
	PG_lru,
	PG_active,
	PG_workingset,
	PG_error,
	PG_slab,
	PG_owner_priv_1,
	PG_arch_1,
	PG_reserved,
	PG_private,
	PG_private_2,
	PG_writeback,
	PG_head,
	PG_mappedtodisk,
	PG_reclaim,
	PG_swapbacked,
	PG_unevictable,
	PG_dropbehind,
	/* Someone is parked waiting for Locked or Writeback to clear. Set by the
	 * waiter before it re-tests; taken by the clearer, which wakes only when
	 * it was set -- the wake is a scan of every task. */
	PG_waiters,
	PG_checked = PG_owner_priv_1,
	PG_owner_2 = PG_private_2,
	PG_readahead = PG_reclaim,
	PG_fscache = PG_private_2,
	__NR_PAGEFLAGS,
};

/* Test, set and clear one of them, atomically. The b1nix side uses these; the
 * Linux spellings in <linux/mm.h> are generated on top of the same bits. */
static inline int lkpi_page_test(const struct page *p, int bit)
{
	return (__atomic_load_n((volatile unsigned long *)&p->flags,
	                        __ATOMIC_ACQUIRE) >> bit) & 1ul;
}
static inline int lkpi_page_test_set(struct page *p, int bit)
{
	unsigned long mask = 1ul << bit;

	return (__atomic_fetch_or((volatile unsigned long *)&p->flags, mask,
	                          __ATOMIC_ACQ_REL) & mask) != 0;
}
static inline int lkpi_page_test_clear(struct page *p, int bit)
{
	unsigned long mask = 1ul << bit;

	return (__atomic_fetch_and((volatile unsigned long *)&p->flags, ~mask,
	                           __ATOMIC_ACQ_REL) & mask) != 0;
}
static inline void lkpi_page_set(struct page *p, int bit)
{ (void)lkpi_page_test_set(p, bit); }
static inline void lkpi_page_clear(struct page *p, int bit)
{ (void)lkpi_page_test_clear(p, bit); }

/* One page. NULL if no frame was available. `gfp` is accepted and ignored for
 * the same reason as in alloc_pages below. */
struct page *lkpi_alloc_page(void);
#define alloc_page(gfp) ({ (void)(gfp); lkpi_alloc_page(); })

/* 2^order physically contiguous pages; returns the head. The array of struct
 * page is contiguous too, so head[i] describes the i-th frame. */
/* `gfp` is the allocation flags upstream callers pass first. b1nix's allocator
 * has one behaviour and no flag changes it, so it is accepted and ignored —
 * kept in the signature because every imported caller passes it, and a
 * one-argument version made each of those a compile error. */
struct page *alloc_pages(u32 gfp, u32 order);

void __free_page(struct page *page);
void __free_pages(struct page *page, u32 order);
/* Make each page of a 2^order run freeable on its own. */
void split_page(struct page *page, unsigned int order);

static inline u64 page_to_phys(const struct page *page)
{
	return page ? page->phys : 0;
}

/* Kernel-visible address of the page through the direct map. Always valid, and
 * independent of any vmap — which is what makes it usable as the second opinion
 * when checking that a vmap really points at the same memory. */
void *page_address(const struct page *page);

/*
 * The frame-to-page direction.
 *
 * b1nix allocates a struct page alongside each frame rather than keeping a
 * mem_map covering all of memory, so this cannot be arithmetic — it is a lookup
 * in a registry every allocator here populates. That means it answers for pages
 * b1nix allocated and for nothing else, which is exactly the set that can reach
 * a driver: a page a driver holds came from one of these allocators.
 *
 * It has to exist. Imported code round-trips a page through its frame number —
 * scatterlists here store a physical address, and i915 walks one with
 * page_to_pfn() followed by pfn_to_page() — so without the inverse, sg_page()
 * answers NULL and the walk silently visits nothing.
 *
 * NULL when the frame was not allocated through b1nix.
 */
struct page *pfn_to_page(unsigned long pfn);

/* The page behind a direct-map address — a kmalloc'd buffer, in practice.
 * NULL for anything else; see the definition for why that is the only honest
 * answer for a vmapped one. */
struct page *lkpi_page_from_virt(const void *addr);

/* Reference counting, for pages shared between a driver and a buffer object. */
void get_page(struct page *page);
/* Returns 1 if this put freed the frame. */
int put_page(struct page *page);

/*
 * A shmem-style backing store: `count` pages, allocated individually so they are
 * not a physical run. Returns an array of `count` page pointers the caller owns,
 * or NULL. Free with shmem_free_pages.
 */
struct page **shmem_alloc_pages(usize count);
void shmem_free_pages(struct page **pages, usize count);

/* Number of physically adjacent neighbours in the array — 0 for a fully
 * scattered allocation. Used by the self-test to assert the scatter is real. */
usize shmem_contiguous_runs(struct page **pages, usize count);

/* Mapping attributes for vmap. */
#define LKPI_PROT_RW  0u /* normal write-back memory */
#define LKPI_PROT_WC  1u /* write-combining, through the M98 PAT setup */

/*
 * Map `count` pages into one linear kernel range. Returns the base, or NULL if
 * the window is full. The pages need not be contiguous — that is the point.
 */
void *lkpi_vmap(struct page **pages, usize count, u32 prot);
void lkpi_vunmap(void *addr);

/* Reserve the vmap window's page-table path. Called once during kernel init,
 * before any process exists, so every address space inherits the entry. */
void lkpi_page_init(void);

/* Pages currently mapped through the vmap window. Diagnostics and self-test. */
usize lkpi_vmap_pages_mapped(void);


/* The window vmap allocates from. Exposed so a caller can tell a vmapped
 * address from a heap one — the test decides between vfree and kfree, so a
 * heuristic would free the wrong way round. */
u64 lkpi_vmap_window_base(void);
u64 lkpi_vmap_window_size(void);

/*
 * A contiguous array of struct page with independently allocated frames.
 *
 * Imported code walks a multi-page scatterlist entry with nth_page(), which is
 * pointer arithmetic on struct page — so the structs must be neighbours even
 * though the frames must not have to be. See the note in page.c for why neither
 * alloc_pages() nor shmem_alloc_pages() gives both.
 *
 * Slots start empty; populate them as they are touched.
 */
struct page *lkpi_pagevec_alloc(usize count);
int lkpi_pagevec_populate(struct page *pv, usize index);
void lkpi_pagevec_release(struct page *pv, usize index);
void lkpi_pagevec_free(struct page *pv, usize count);


/* Registry maintenance, for allocators here. Every page handed out is
 * registered so pfn_to_page() can find it, and unregistered before its frame
 * goes back. */
void lkpi_page_register(struct page *page);
void lkpi_page_unregister(struct page *page);

#endif
