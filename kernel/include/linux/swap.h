/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SWAP_H
#define LKPI_LINUX_SWAP_H
#include <linux/types.h>
/* What a driver reads from here is how much memory it may reasonably pin. b1nix
 * has swap, but no page-cache accounting a driver could interrogate, so the
 * answer is the conservative one: report nothing available rather than a number
 * nothing computed, so a shrinker sized from this errs towards freeing. */
static inline long get_nr_swap_pages(void) { return 0; }
/* Total RAM in pages, which a shrinker sizes its target against. Real: it
 * comes from b1nix's physical allocator, so a driver that scales a cache to
 * a fraction of memory gets the fraction it asked for. */
unsigned long totalram_pages(void);

/* The batch size reclaim works in. b1nix's reclaim uses its own batching; this
 * is the number a driver's shrinker rounds its request up to. */
#define SWAP_CLUSTER_MAX 32ul


/* Is the calling thread the page-reclaim daemon? b1nix has a kswapd; a driver
 * shrinker asks so it can avoid recursing into itself. */
bool current_is_kswapd(void);


/* Reclaim's view of a mapping travels with this header for the sources that
 * shrink one; i915's shmem backend calls mapping_clear_unevictable() with only
 * <linux/swap.h> in scope. */
#include <linux/pagemap.h>

#endif
