/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_HIGHMEM_H
#define LKPI_LINUX_HIGHMEM_H
#include <linux/mm.h>
#include <lkpi/page.h>
/* On 32-bit Linux a page might not be permanently mapped, so it had to be
 * kmapped for access. b1nix is 64-bit with a full direct map, so every page is
 * always addressable and these are just page_address — no temporary mapping to
 * balance, and nothing to leak by forgetting to unmap. */
static inline void *kmap_local_page(struct page *p) { return page_address(p); }
static inline void kunmap_local(const void *addr) { (void)addr; }
static inline void *kmap_atomic(struct page *p) { return page_address(p); }
static inline void kunmap_atomic(const void *addr) { (void)addr; }
static inline void *kmap(struct page *p) { return page_address(p); }
static inline void kunmap(struct page *p) { (void)p; }


/* Map a page with a chosen protection. Every page is in the direct map here, so
 * the mapping already exists and the protection is the direct map's — a caller
 * asking for write-combining on a temporary mapping gets cached instead, which
 * is slower for a bulk copy and not wrong. */
#define kmap_local_page_prot(page, prot) ({ (void)(prot); kmap_local_page(page); })

/*
 * The folio spelling, with a byte offset into the folio rather than a page.
 *
 * The offset matters: upstream a folio can be several pages, so the mapping
 * covers only the page the offset falls in and the caller may use at most to
 * the end of it. One page per folio here makes that the whole folio, but the
 * offset is still added — a version that ignored it would return the start of
 * the folio and every caller would read from the wrong place.
 */
static inline void *kmap_local_folio(struct folio *folio, size_t offset)
{
	return (char *)page_address(folio_page(folio, 0)) + offset;
}

/* Zero and copy helpers that take the mapping into account, so a caller does
 * not have to map and unmap around a memset. */
void memzero_page(struct page *page, size_t offset, size_t len);
void memcpy_to_page(struct page *page, size_t offset, const char *from,
                    size_t len);
void memcpy_from_page(char *to, struct page *page, size_t offset, size_t len);

#endif
