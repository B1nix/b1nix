/* SPDX-License-Identifier: MIT */
#ifndef LKPI_ASM_CACHEFLUSH_H
#define LKPI_ASM_CACHEFLUSH_H
#include <linux/io.h>
/* The cache maintenance a driver needs on x86_64 is clflush over a range, which
 * <linux/io.h> already exposes through b1nix's M98 paths. The kernel-mapping
 * flushes upstream also declares here have no meaning on a coherent x86. */

/* Flush the caches for a range of a userspace mapping. x86 caches are coherent
 * with DMA and with other cores, and b1nix does not alias kernel and user
 * mappings of the same page with different attributes, so there is nothing this
 * needs to do on this architecture — which is also why upstream's x86 version
 * is empty. */
struct vm_area_struct;
static inline void flush_cache_range(struct vm_area_struct *vma,
                                     unsigned long start, unsigned long end)
{ (void)vma; (void)start; (void)end; }

#endif
