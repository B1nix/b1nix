/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_SET_MEMORY_H
#define LKPI_ASM_SET_MEMORY_H
#include <lkpi/env.h>
#include <linux/types.h>
/*
 * Changing the caching attributes of the kernel's own mapping of a page.
 *
 * i915 uses this so the CPU's view of a buffer matches the GPU's. b1nix does
 * have per-mapping attributes (M98's PAT work), but changing them on the direct
 * map after the fact is not implemented — and quietly returning success would
 * leave a write-back kernel alias of a write-combining buffer, which is exactly
 * the aliasing the call exists to prevent. So these report failure, and a caller
 * that needs them fails visibly instead of corrupting a frame now and then.
 */
int set_memory_wc(unsigned long addr, int numpages);
int set_memory_wb(unsigned long addr, int numpages);
int set_memory_uc(unsigned long addr, int numpages);
int set_pages_array_wc(struct page **pages, int addrinarray);
int set_pages_array_wb(struct page **pages, int addrinarray);

static inline bool pat_enabled(void) { return lkpi_pat_enabled() != 0; }


/* The same, in the spellings TTM's page pool uses. Same answer and the same
 * reason: b1nix cannot change the direct map's caching after the fact, and
 * reporting success would leave a write-back alias of memory a device is about
 * to read uncached. TTM treats a failure as "this pool cannot hold non-WB
 * pages" and keeps them write-back, which is coherent because nothing here
 * bypasses the caches without flushing (see <asm/cacheflush.h>). */
int set_pages_wb(struct page *page, int numpages);
int set_pages_uc(struct page *page, int numpages);
int set_pages_array_uc(struct page **pages, int addrinarray);

#endif
