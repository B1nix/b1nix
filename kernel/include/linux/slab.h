/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SLAB_H
#define LKPI_LINUX_SLAB_H

#include <linux/types.h>
#include <lkpi/types.h>

/*
 * Allocation, onto b1nix's kheap through lkpi.
 *
 * Real functions rather than macros over b1nix's own kmalloc. b1nix has a
 * kmalloc too, taking one argument where Linux's takes two, and the macro
 * version only worked as long as b1nix's header was included first — an
 * ordering nothing enforced and every new include could break. It is not
 * included here at all now.
 *
 * The slab caches Linux keeps for object reuse are not reproduced: kheap
 * already coalesces, and a cache layer that only forwarded would be a lie about
 * where the memory comes from.
 */

static inline void *kmalloc(usize size, gfp_t flags)
{
	return lkpi_kmalloc(size, flags);
}

static inline void *kzalloc(usize size, gfp_t flags)
{
	return lkpi_kmalloc(size, flags | __GFP_ZERO);
}

static inline void *kcalloc(usize n, usize size, gfp_t flags)
{
	return lkpi_kcalloc(n, size, flags);
}

static inline void *kmalloc_array(usize n, usize size, gfp_t flags)
{
	return lkpi_kcalloc(n, size, flags);
}

static inline void kfree(const void *ptr) { lkpi_kfree((void *)ptr); }

static inline void *kvzalloc(usize size, gfp_t flags)
{
	return lkpi_kmalloc(size, flags | __GFP_ZERO);
}

static inline void *kvmalloc_array(usize n, usize size, gfp_t flags)
{
	return lkpi_kcalloc(n, size, flags);
}

static inline void *kvcalloc(usize n, usize size, gfp_t flags)
{
	return lkpi_kcalloc(n, size, flags);
}

static inline void kvfree(const void *ptr) { lkpi_kfree((void *)ptr); }

/* krealloc is out of line: kheap cannot resize, so it is allocate-copy-free and
 * the copy has to be accounted for somewhere a caller can read about it. */
void *krealloc(const void *p, usize new_size, gfp_t flags);
void *kmemdup(const void *src, usize len, gfp_t flags);

/* Grow an array allocation. Same allocate-copy-free as krealloc, with the
 * element count multiplied out under an overflow check. */
static inline void *krealloc_array(void *p, usize n, usize size, gfp_t flags)
{
	usize bytes;
	if (__builtin_mul_overflow(n, size, &bytes))
		return 0;
	return krealloc(p, bytes, flags);
}

/* Linux distinguishes a string that may be a compile-time constant from one on
 * the heap. b1nix never allocates the constant kind, so the free is a plain
 * free and the distinction costs nothing. */
static inline void kfree_const(const void *p) { lkpi_kfree((void *)p); }

char *kasprintf(gfp_t flags, const char *fmt, ...);
char *kstrdup(const char *s, gfp_t flags);
/* Linux distinguishes a duplicate of a possibly-constant string. b1nix never
 * allocates the constant kind, so it is a plain duplicate. */
static inline char *kstrdup_const(const char *s, gfp_t flags)
{
	return kstrdup(s, flags);
}
/* Copy a buffer in from userspace, allocating for it. Returns ERR_PTR on
 * failure, which is why callers test it with IS_ERR rather than for NULL. */
void *memdup_user(const void *user_src, usize len);

/* NUMA-aware allocation. b1nix has one node, so the node argument selects
 * nothing and the caller-tracking is not recorded. */
#define kmalloc_node_track_caller(size, flags, node) \
	lkpi_kmalloc((size), (flags))
#define kmalloc_node(size, flags, node) lkpi_kmalloc((size), (flags))
char *kvasprintf(gfp_t flags, const char *fmt, __builtin_va_list ap);

/* Usable size of an allocation. kheap does not report it, so this returns the
 * size asked for — never more, so a caller that writes up to ksize() stays
 * inside its own block. */
static inline usize ksize(const void *p) { (void)p; return 0; }

#define ARCH_KMALLOC_MINALIGN 8

#endif
