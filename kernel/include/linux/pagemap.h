/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_PAGEMAP_H
#define LKPI_LINUX_PAGEMAP_H
#include <linux/mm.h>
#include <linux/types.h>
/* The page cache's index-to-page mapping. b1nix has a page cache of its own
 * (kernel/mm/page_cache.c) keyed by (fs_id, ino); bridging the two is decided
 * when a driver actually backs an object with a file rather than with anonymous
 * pages. */
struct address_space;
/* Mark a mapping's pages un-evictable while a driver has them pinned. b1nix's
 * reclaim does not consult a per-mapping flag, and nothing pins these pages
 * behind its back, so there is no state to set. */
/* Allocation flags recorded on a mapping, so pages faulted into it come from
 * the right zone. b1nix has one zone; the answer is the ordinary kernel
 * context. */
static inline gfp_t mapping_gfp_mask(struct address_space *m)
{ (void)m; return GFP_KERNEL; }
static inline gfp_t mapping_gfp_constraint(struct address_space *m, gfp_t gfp)
{ (void)m; return gfp; }

static inline void mapping_set_unevictable(struct address_space *m) { (void)m; }
static inline void mapping_clear_unevictable(struct address_space *m) { (void)m; }

static inline void mapping_set_gfp_mask(struct address_space *m, gfp_t mask)
{ (void)m; (void)mask; }
#endif
