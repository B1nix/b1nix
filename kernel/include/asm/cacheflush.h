/* SPDX-License-Identifier: GPL-2.0-only */
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


/*
 * Cache-line flush.
 *
 * The display engine is not coherent with the CPU's caches for a scanout
 * buffer: i915 writes a frame with the CPU and relies on drm_clflush_* to push
 * those lines out before the hardware fetches them. Without these, drm_cache.c
 * compiles its "this architecture cannot flush" branch and does nothing at all
 * — which it says once per call, and said 13000 times in one boot.
 *
 * clflushopt is the unordered form; ordering is the caller's business, which is
 * why drm_cache.c brackets its loops with mb().
 */
static inline void clflush(volatile void *p)
{ __asm__ volatile("clflush %0" : "+m"(*(volatile char *)p)); }

static inline void clflushopt(volatile void *p)
{
	/*
	 * Plain CLFLUSH. CLFLUSHOPT differs only in being weakly ordered, which
	 * makes it faster in a loop and requires the caller to fence — and every
	 * caller here already fences, because it has to for CLFLUSHOPT. CLFLUSH is
	 * ordered, so it satisfies those callers too, and it needs no hand-encoded
	 * prefix or a CPU feature test to be safe.
	 */
	clflush(p);
}

static inline void clflush_cache_range(void *addr, unsigned int size)
{
	const int line = 64;
	char *p = (char *)addr;
	char *end = p + size;

	__asm__ volatile("mfence" ::: "memory");
	for (; p < end; p += line)
		clflush(p);
	__asm__ volatile("mfence" ::: "memory");
}

#endif
