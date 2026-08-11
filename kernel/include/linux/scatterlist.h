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

/*
 * Chained scatter tables.
 *
 * Upstream stores a table longer than one allocation as a chain: the last entry
 * of a block points at the next block instead of at a page, with the low bits
 * of `page_link` saying which it is. b1nix's tables carry no such link — they
 * are allocated whole, and `struct scatterlist` here has no page_link field at
 * all — so a table built by this kernel is never chained and the last entry is
 * the one at index nents-1.
 *
 * That is what these answer. They are not upstream's bit tests with a field
 * renamed: there is no encoded bit to test, and inventing one would be claiming
 * a representation b1nix does not use. A table that genuinely needed chaining
 * would have to change <lkpi/scatterlist.h> first, and these with it.
 */
static inline bool sg_is_chain(struct scatterlist *sg)
{ (void)sg; return false; }
static inline struct scatterlist *sg_chain_ptr(struct scatterlist *sg)
{ (void)sg; return 0; }
/* The end marker lives in the entry — see <lkpi/scatterlist.h> for why it has
 * to, and who maintains it. */
static inline bool sg_is_last(struct scatterlist *sg)
{ return sg && sg->end; }


/* Building a table entry from a page. b1nix's entries carry the frame rather
 * than a page pointer, so this records the same fact in the form the table
 * uses; sg_page above is the direction that cannot be answered, and says so. */
static inline void sg_set_page(struct scatterlist *sg, struct page *page,
                               unsigned int len, unsigned int offset)
{
	sg->phys = page_to_phys(page);
	sg->offset = offset;
	sg->length = len;
	sg->end = 0;
}

static inline void sg_mark_end(struct scatterlist *sg)
{ if (sg) sg->end = 1; }

static inline void sg_unmark_end(struct scatterlist *sg)
{ if (sg) sg->end = 0; }

/* The next entry, or NULL at the end. Entries are contiguous in b1nix's tables
 * — there is no chaining — so "next" is the neighbouring element. */
static inline struct scatterlist *sg_next(struct scatterlist *sg)
{ return (!sg || sg_is_last(sg)) ? 0 : sg + 1; }


/* Point an entry at a page without touching its length. Used when a table is
 * built in two passes — the geometry first, the pages after. */
static inline void sg_assign_page(struct scatterlist *sg, struct page *page)
{ if (sg) sg->phys = page_to_phys(page); }


/* Point an entry at a folio. One page per folio here — see <linux/mm.h> — so
 * this is sg_set_page on that page. */
struct folio;
static inline void sg_set_folio(struct scatterlist *sg, struct folio *folio,
                                usize len, usize offset)
{ sg_set_page(sg, (struct page *)folio, len, offset); }

#endif
