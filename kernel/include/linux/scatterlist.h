/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SCATTERLIST_H
#define LKPI_LINUX_SCATTERLIST_H
#include <lkpi/scatterlist.h>
#include <linux/mm.h>
#include <linux/types.h>

/* The page iterator lives in <lkpi/scatterlist.h>, so its implementation
 * file needs no linux header — see <lkpi/env.h> for the boundary rule. */

/* Accessors for one entry. Named as macros because imported code uses them on
 * both a scatterlist element and a pointer to one. */
#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg)     ((sg)->length)
#define sg_page(sg)        ((struct page *)0)
#define for_each_sgtable_dma_sg(sgt, sg, i) \
	for ((i) = 0, (sg) = (sgt)->sgl; (i) < (int)(sgt)->nents; (i)++, (sg)++)
#define for_each_sgtable_sg(sgt, sg, i) for_each_sgtable_dma_sg(sgt, sg, i)

/*
 * Page-granular loops. An entry can cover several pages after coalescing, so
 * these walk pages rather than entries — which is what a caller filling a page
 * array needs. The iterator itself lives in <lkpi/scatterlist.h>; see
 * <lkpi/env.h> for why its implementation stays on that side.
 */
#define for_each_sgtable_page(sgt, piter, pgoffset)      \
	for (__sg_page_iter_start((piter), (sgt)); __sg_page_iter_next(piter);)

#define for_each_sgtable_dma_page(sgt, dma_iter, pgoffset)   \
	for (__sg_page_iter_start(&(dma_iter)->base, (sgt));     \
	     __sg_page_iter_next(&(dma_iter)->base);)
#define for_each_sg(sgl, sg, nr, i) \
	for ((i) = 0, (sg) = (sgl); (i) < (int)(nr); (i)++, (sg)++)

/*
 * Linux's form takes a page array; lkpi's takes the frame addresses, because
 * that is what its sg entries hold. The conversion is out of line rather than
 * a macro, so the page array is walked once in a place that can report failure.
 */
int lkpi_sg_alloc_table_from_page_array(struct sg_table *sgt,
                                        struct page **pages,
                                        unsigned int n_pages,
                                        unsigned int offset,
                                        unsigned long size, gfp_t gfp);

/* Defined after the lkpi declaration above is already in scope, so the two
 * spellings of the name do not collide. */
#define sg_alloc_table_from_pages(sgt, pages, n, off, size, gfp) \
	lkpi_sg_alloc_table_from_page_array((sgt), (pages), (n), (off), (size), (gfp))
#define sg_alloc_table_from_pages_segment(sgt, pages, n, off, size, seg, gfp) \
	lkpi_sg_alloc_table_from_page_array((sgt), (pages), (n), (off), (size), (gfp))

/* sg_alloc_table_from_pages is declared by <lkpi/scatterlist.h>, which this
 * header includes; re-declaring it here with a different spelling of the same
 * types is a conflict, not a convenience. */
/* Onto lkpi's scatterlists (M99), which already coalesce adjacent runs and
 * carry both the physical address and the device address per entry. */
#endif
