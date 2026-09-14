/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_GFP_H
#define LKPI_LINUX_GFP_H
#include <lkpi/page.h> /* PAGE_SIZE, for get_order below */
#include <lkpi/types.h>
#include <linux/types.h>
/* Allocation flags. b1nix's kmalloc never sleeps and never blocks, so the
 * distinction that matters on Linux — may this allocation sleep — does not
 * exist here; the flags are accepted so callers compile, and __GFP_ZERO is
 * honoured because it changes the result. */
#define __GFP_NOFAIL  0x1000u
#define __GFP_ZERO_ALIAS __GFP_ZERO
#define __GFP_NOFAIL  0x1000u
#define __GFP_HIGHMEM 0x0800u
#define __GFP_DMA32   0x1000u
#define GFP_USER      GFP_KERNEL
#define GFP_HIGHUSER  GFP_KERNEL

/* The allocation order that covers `size` bytes: the smallest n with
 * 2^n pages >= size. Written as a loop rather than through a log2 so that
 * size 0 gives 0 rather than an undefined shift. */
static inline unsigned int get_order(unsigned long size)
{
	unsigned int order = 0;
	unsigned long pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

	while ((1UL << order) < pages)
		order++;
	return order;
}


/* Allocation hints b1nix's allocator does not act on: it never reclaims and has
 * no zones to steer between, so these select nothing. Defined because callers
 * name them, and accepting a hint that changes nothing is honest where
 * pretending to honour it would not be. */
#define __GFP_RECLAIMABLE 0
#define __GFP_MOVABLE     0
#define __GFP_COMP        0
#define __GFP_NOWARN      0
#define __GFP_RETRY_MAYFAIL 0
#define __GFP_NORETRY     0


#define __GFP_RECLAIM 0
#define __GFP_DIRECT_RECLAIM 0
#define __GFP_KSWAPD_RECLAIM 0


/* Whether an allocation with these flags may sleep: all but GFP_ATOMIC and
 * GFP_NOWAIT. Callers use the answer to decide whether to cond_resched() or
 * take a sleeping lock while a spinlock is held -- getting it wrong is a
 * deadlock, not a slowdown. */
static inline bool gfpflags_allow_blocking(gfp_t flags)
{ return (flags & (GFP_ATOMIC | GFP_NOWAIT)) == 0; }


/* Do not dip into emergency reserves for this allocation. b1nix's allocator
 * keeps no reserve to dip into, so the flag describes the only behaviour. */
#ifndef __GFP_NOMEMALLOC
#define __GFP_NOMEMALLOC 0x00020000u
#endif

/*
 * The reclaim-recursion flags. __GFP_FS says the allocator MAY call back into a
 * filesystem to free memory; a filesystem holding a transaction open must clear
 * it (GFP_NOFS) or reclaim re-enters underneath it and deadlocks on the lock it
 * already holds. __GFP_IO is the same statement one layer down.
 *
 * b1nix's reclaim does not call into a filesystem today, so clearing the flag
 * changes nothing yet. The values still have to be distinct bits: the
 * filesystems mask them in and out and compare the results.
 */
#ifndef __GFP_IO
#define __GFP_IO   0x0040u
#endif
#ifndef __GFP_FS
#define __GFP_FS   0x0080u
#endif
#ifndef __GFP_HIGHMEM
#define __GFP_HIGHMEM 0x0002u
#endif
#ifndef __GFP_MOVABLE
#define __GFP_MOVABLE 0x0008u
#endif
#ifndef __GFP_HARDWALL
#define __GFP_HARDWALL 0x100000u
#endif
#ifndef GFP_NOFS
#define GFP_NOFS   (GFP_KERNEL & ~__GFP_FS)
#endif
#ifndef GFP_NOIO
#define GFP_NOIO   (GFP_KERNEL & ~(__GFP_FS | __GFP_IO))
#endif
#ifndef GFP_HIGHUSER
#define GFP_HIGHUSER (GFP_KERNEL | __GFP_HIGHMEM)
#endif
#ifndef GFP_HIGHUSER_MOVABLE
#define GFP_HIGHUSER_MOVABLE (GFP_HIGHUSER | __GFP_MOVABLE)
#endif

/* One past the highest allocation-flag bit used above (__GFP_WRITE, bit 23).
 * The xarray keeps its per-array mark flags in the bits from here up, so a
 * new flag above bit 23 must move this too. */
/* Access to emergency reserves, and allocation pinned to the given node.
 * Neither changes what the heap does here; distinct bits keep masks honest. */
#define __GFP_HIGH     0x00040000u
#define __GFP_THISNODE 0x00080000u

#define __GFP_BITS_SHIFT 24
#define __GFP_BITS_MASK  ((gfp_t)((1u << __GFP_BITS_SHIFT) - 1))
/* The zone modifiers, which say where memory comes from rather than how the
 * allocation behaves; a radix tree strips them from the mask it stores. */
#define GFP_ZONEMASK     (__GFP_DMA32 | __GFP_HIGHMEM | __GFP_MOVABLE)
/* Memory-cgroup accounting, which b1nix does not have. */
#define __GFP_ACCOUNT    0

#endif
