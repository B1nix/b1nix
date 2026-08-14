/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_TYPES_H
#define LKPI_TYPES_H

#include <b1nix/types.h>

/*
 * Only <b1nix/types.h> is included, and only because it is pure typedefs.
 *
 * Nothing here may pull in a b1nix header that declares functions or macros: a
 * translation unit compiling imported DRM source includes this one, and every
 * name b1nix and Linux share with different meanings — kmalloc, spin_lock,
 * ERR_PTR — becomes a conflict resolved by include order if it does.
 */

/* Fixed-width aliases drivers spell the Linux way. b1nix's own u8/u32/... come
 * from <b1nix/types.h>; these are the __-prefixed and signed spellings. */
typedef u8 lkpi_u8;
typedef u16 lkpi_u16;
typedef u32 lkpi_u32;
typedef u64 lkpi_u64;
typedef i32 lkpi_s32;
typedef i64 lkpi_s64;

/* A device-visible address. b1nix has no IOMMU, so a dma_addr_t is always the
 * physical address of the page — but drivers must still go through the
 * dma-mapping API, because that is where an IOMMU would be inserted later. */
typedef u64 dma_addr_t;

/* Allocation context. b1nix's kmalloc never sleeps and never blocks, so the
 * distinction that matters in Linux (may this allocation sleep?) does not exist
 * here. The flags are accepted so driver code compiles unchanged, and GFP_ZERO
 * is honoured because it changes the result. */
typedef unsigned int gfp_t;
#define GFP_KERNEL 0x0000u
#define GFP_ATOMIC 0x0001u
#define GFP_NOWAIT 0x0002u
#define GFP_DMA32  0x0004u
#define __GFP_ZERO 0x0100u

/* Error pointers. b1nix already uses IS_ERR/ERR_PTR in the VFS; repeat the
 * spelling drivers expect. */
#ifndef LKPI_MAX_ERRNO
#define LKPI_MAX_ERRNO 4095
#endif

static inline void *lkpi_err_ptr(long error)
{
	return (void *)(usize)error;
}

static inline long lkpi_ptr_err(const void *ptr)
{
	return (long)(usize)ptr;
}

static inline int lkpi_is_err(const void *ptr)
{
	return (usize)ptr >= (usize)-LKPI_MAX_ERRNO;
}

#define LKPI_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define LKPI_MIN(a, b) ((a) < (b) ? (a) : (b))
#define LKPI_MAX(a, b) ((a) > (b) ? (a) : (b))
#define LKPI_ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))

void *lkpi_kmalloc(usize size, gfp_t flags);
void *lkpi_kcalloc(usize n, usize size, gfp_t flags);
void lkpi_kfree(void *ptr);

#endif
