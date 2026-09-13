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


#ifndef __GFP_ATOMIC
#define __GFP_ATOMIC 0x80u
#endif


/* Whether an allocation with these flags may sleep. Everything here can except
 * GFP_ATOMIC, and callers use the answer to decide whether to take a lock
 * first — so getting it wrong is a deadlock, not a slowdown. */
static inline bool gfpflags_allow_blocking(gfp_t flags)
{ return (flags & __GFP_ATOMIC) == 0; }


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
#ifndef GFP_NOWAIT
#define GFP_NOWAIT (__GFP_NOWARN)
#endif
#ifndef GFP_HIGHUSER
#define GFP_HIGHUSER (GFP_KERNEL | __GFP_HIGHMEM)
#endif
#ifndef GFP_HIGHUSER_MOVABLE
#define GFP_HIGHUSER_MOVABLE (GFP_HIGHUSER | __GFP_MOVABLE)
#endif

#endif
