/* SPDX-License-Identifier: MIT */
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
#endif
