/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BVEC_H
#define LKPI_LINUX_BVEC_H

#include <linux/types.h>
#include <linux/mm.h>

/*
 * The bio vector: one contiguous run of bytes inside one page.
 *
 * Everything the block layer moves is described as an array of these, and the
 * three fields are the whole of it — which page, how many bytes, and where in
 * the page they start. A run that crosses a page boundary is two entries, never
 * one, and code that assumes otherwise breaks on the first unaligned write.
 */
struct bio_vec {
	struct page *bv_page;
	unsigned int bv_len;
	unsigned int bv_offset;
};

struct bvec_iter;

static inline void bvec_set_page(struct bio_vec *bv, struct page *page,
                                 unsigned int len, unsigned int offset)
{
	bv->bv_page = page;
	bv->bv_len = len;
	bv->bv_offset = offset;
}

static inline void bvec_set_folio(struct bio_vec *bv, struct folio *folio,
                                  unsigned int len, unsigned int offset)
{
	bvec_set_page(bv, folio_page(folio, 0), len, offset);
}

static inline struct page *bvec_page(const struct bio_vec *bv)
{
	return bv->bv_page;
}

#endif
