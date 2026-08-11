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

/* Flags on a file's page cache that say how its pages may be reclaimed.
 * b1nix's GEM pages have no address_space behind them — see
 * <linux/shmem_fs.h> — so there is nothing to mark. */
#define mapping_set_unevictable(m)   do { (void)(m); } while (0)
#define mapping_clear_unevictable(m) do { (void)(m); } while (0)
#define mapping_set_gfp_mask(m, g)   do { (void)(m); (void)(g); } while (0)
#define mapping_gfp_mask(m)          ({ (void)(m); (gfp_t)0; })
#define mapping_gfp_constraint(m, g) ({ (void)(m); (g); })


/* Find a page-cache page and return it locked. b1nix's page cache is keyed by
 * (fs_id, ino) and is not reachable from an address_space pointer — see the
 * note at the top of this header — so this is declared and not defined; a
 * caller fails to link rather than being told the page is absent when it is
 * not. */
struct page *find_lock_page(struct address_space *mapping, unsigned long index);


/* Page-cache page locking. b1nix's driver pages are not in a page cache and are
 * never looked up by index behind the driver's back, so there is no window a
 * lock would close — these are the operation upstream's callers bracket their
 * work with, and here the bracket is empty. */
static inline void lock_page(struct page *page) { (void)page; }
static inline void unlock_page(struct page *page) { (void)page; }
static inline int trylock_page(struct page *page) { (void)page; return 1; }

#endif
