/* SPDX-License-Identifier: MIT */
#ifndef LKPI_SCATTERLIST_H
#define LKPI_SCATTERLIST_H

#include <lkpi/types.h>

/*
 * scatterlist — a buffer described as a list of physically discontiguous runs.
 *
 * This is the type that makes discontiguous GPU buffer objects possible: a BO
 * is allocated page by page from the frame allocator and then described by an
 * sg table, which the device's page-table/GTT code walks. Nothing in the GPU
 * path may assume one contiguous allocation, because a fragmented system
 * cannot produce one.
 *
 * Adjacent pages are coalesced into a single entry as they are appended, so a
 * table built from a lucky contiguous run costs one entry, not one per page.
 * `nents` is the number of entries actually used.
 */

struct scatterlist {
	u64 phys;   /* physical start of this run */
	u32 offset; /* byte offset into the first page of the run */
	u32 length; /* bytes in this run */
	/* Address the DEVICE uses for this run, filled in by dma_map_sg. Equal to
	 * phys + offset on a system without an IOMMU and a device that can reach
	 * the run; different when the run had to be bounced (see
	 * dma_map_sg_masked). Meaningless before the table is mapped. */
	dma_addr_t dma_address;
};

struct sg_table {
	struct scatterlist *sgl;
	u32 nents;      /* entries in use */
	u32 orig_nents; /* entries allocated */
	u64 total_bytes;
};

/* Allocate a table able to hold `max_ents` entries. Returns 0 or -ENOMEM. */
int sg_alloc_table(struct sg_table *sgt, u32 max_ents);
void sg_free_table(struct sg_table *sgt);

/* Append one physical run, coalescing with the previous entry when the two are
 * adjacent. Returns 0, -ENOSPC when the table is full, -EINVAL on a zero
 * length. */
int sg_append(struct sg_table *sgt, u64 phys, u32 offset, u32 length);

/* Build a table from an array of page frames (each PAGE_SIZE bytes), coalescing
 * runs. Convenience over sg_append for the GEM allocator. */
int sg_alloc_table_from_pages(struct sg_table *sgt, const u64 *frames,
                              u32 nframes);

/* Physical address of `offset` bytes into the buffer the table describes, or 0
 * when the offset is past the end. This is the lookup a page-fault or mmap path
 * performs per page. */
u64 sg_phys_at(const struct sg_table *sgt, u64 offset);

/* 1 when the whole table describes one contiguous physical run. */
int sg_is_contiguous(const struct sg_table *sgt);

/* Copy `len` bytes from the buffer the table describes into `buf`, walking
 * entries. Returns bytes copied. Reads through the direct map, so it requires
 * every run to lie inside it (true for pmm-allocated frames). */
usize sg_copy_to_buffer(const struct sg_table *sgt, u64 offset, void *buf,
                        usize len);
/* The reverse. */
usize sg_copy_from_buffer(const struct sg_table *sgt, u64 offset,
                          const void *buf, usize len);


/*
 * Page-granular iteration over a table. An entry may cover several pages after
 * coalescing, so this walks pages rather than entries — which is what a caller
 * filling a page array needs, and why it is not a loop over sgl.
 */
struct sg_page_iter {
	struct sg_table *sgt;
	unsigned int entry;
	unsigned int page_in_entry;
};

struct sg_dma_page_iter {
	struct sg_page_iter base;
};

void __sg_page_iter_start(struct sg_page_iter *iter, struct sg_table *sgt);
_Bool __sg_page_iter_next(struct sg_page_iter *iter);
dma_addr_t sg_page_iter_dma_address(struct sg_dma_page_iter *iter);
struct page *sg_page_iter_page(struct sg_page_iter *iter);

#endif
