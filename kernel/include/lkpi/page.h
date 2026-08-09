/* SPDX-License-Identifier: MIT */
#ifndef LKPI_PAGE_H
#define LKPI_PAGE_H

#include <lkpi/types.h>

/* The page size, defined on this side of the boundary so <linux/mm.h> does not
 * have to reach into <b1nix/mm.h> for it. */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096u
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

struct page {
	u64 phys;            /* physical address of the frame */
	volatile i32 count;  /* references; the frame is freed when it hits 0 */
	u32 order;           /* set on the head page of an alloc_pages run */
};

/* One page. NULL if no frame was available. */
struct page *alloc_page(void);

/* 2^order physically contiguous pages; returns the head. The array of struct
 * page is contiguous too, so head[i] describes the i-th frame. */
struct page *alloc_pages(u32 order);

void __free_page(struct page *page);
void __free_pages(struct page *page, u32 order);

static inline u64 page_to_phys(const struct page *page)
{
	return page ? page->phys : 0;
}

/* Kernel-visible address of the page through the direct map. Always valid, and
 * independent of any vmap — which is what makes it usable as the second opinion
 * when checking that a vmap really points at the same memory. */
void *page_address(const struct page *page);

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

#endif
