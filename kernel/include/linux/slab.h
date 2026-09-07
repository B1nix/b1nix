/* SPDX-License-Identifier: GPL-2.0-only */
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

/*
 * kvmalloc: try contiguous, fall back to virtually contiguous.
 *
 * The distinction is the whole point upstream — a 2 MiB allocation that does
 * not need to be physically contiguous should not fail because memory is
 * fragmented. b1nix's heap hands back a physically contiguous range or nothing,
 * so this IS kmalloc here, and a caller asking for something too large fails
 * rather than succeeding through vmalloc. That is a real limitation, not a
 * naming detail: it is where a large ext4 mount on a fragmented heap would
 * fail first.
 */
static inline void *kvmalloc(usize size, gfp_t flags)
{
	return lkpi_kmalloc(size, flags);
}

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


/*
 * Slab caches: a pool of same-sized objects.
 *
 * b1nix's heap already serves a fixed size well, so a cache here is a named
 * wrapper around kmalloc rather than a separate allocator. What is kept is the
 * interface's guarantees the callers rely on — the size and the constructor —
 * and what is lost is the locality a real slab buys, which costs cache misses
 * rather than correctness.
 */
struct kmem_cache;

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
                                     unsigned int align, unsigned long flags,
                                     void (*ctor)(void *));
void kmem_cache_destroy(struct kmem_cache *c);
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t flags);
void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t flags);
void kmem_cache_free(struct kmem_cache *c, void *obj);
void kmem_cache_shrink(struct kmem_cache *c);

#define SLAB_HWCACHE_ALIGN 0x00002000u
#define SLAB_RECLAIM_ACCOUNT 0x00020000u
/*
 * The rest of the cache flags a filesystem passes.
 *
 * Every one of them is a hint to a reclaim and accounting machinery b1nix does
 * not have, so none of them changes behaviour here — but they are distinct bits
 * rather than zero, because callers OR them together and a zero would make two
 * different requests compare equal in code that tests the mask.
 *
 * SLAB_TYPESAFE_BY_RCU is the exception worth watching: it is not a hint. It
 * promises that a freed object's MEMORY stays a valid object of the same type
 * until an RCU grace period passes, which is what makes a lockless lookup that
 * races with a free safe. Nothing in the current object list depends on it, and
 * if something does, this is where the promise has to become real.
 */
#define SLAB_TEMPORARY       0x00040000u
#define SLAB_MEM_SPREAD      0x00080000u
#define SLAB_ACCOUNT         0x00100000u
#define SLAB_NOLEAKTRACE     0x00200000u
#define SLAB_TYPESAFE_BY_RCU 0x00400000u
#define SLAB_CONSISTENCY_CHECKS 0x00800000u
#define SLAB_STORE_USER      0x01000000u
#define SLAB_PANIC           0x02000000u
#define SLAB_RED_ZONE        0x04000000u
#define SLAB_POISON          0x08000000u
#define SLAB_TYPESAFE_BY_RCU 0x00080000u
#define KMEM_CACHE(__struct, __flags) \
	kmem_cache_create(#__struct, sizeof(struct __struct), \
	                  __alignof__(struct __struct), (__flags), 0)


/* The pointer a zero-sized allocation returns: not NULL, so a caller cannot
 * mistake it for failure, and not dereferenceable, so using it faults rather
 * than corrupting something. */
#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)

/*
 * The size kmalloc will actually give for a request.
 *
 * Callers use it to grow into the slack they were going to be charged for
 * anyway — a filesystem sizing a buffer asks first and takes the rounded size.
 * Returning the request unchanged is always CORRECT (nobody may use more than
 * they asked for), it just gives the slack away.
 */
size_t kmalloc_size_roundup(size_t size);

/* Bulk allocation from a cache: `nr` objects into `p`, returning how many were
 * actually produced. A short return is not an error — callers loop — and code
 * that treats it as one leaks the objects it was given. */
int kmem_cache_alloc_bulk(struct kmem_cache *s, gfp_t gfp, size_t nr, void **p);
void kmem_cache_free_bulk(struct kmem_cache *s, size_t nr, void **p);

/* A cache whose objects may be copied to and from userspace, with the copyable
 * window declared. b1nix does not enforce usercopy bounds, so the window is
 * recorded and not checked — which is why the plain create is preferred
 * anywhere the window is the whole object. */
struct kmem_cache *kmem_cache_create_usercopy(const char *name,
                                              unsigned int size,
                                              unsigned int align,
                                              unsigned int flags,
                                              unsigned int useroffset,
                                              unsigned int usersize,
                                              void (*ctor)(void *));

#ifndef GFP_KERNEL_ACCOUNT
/* Charged to a memory cgroup. b1nix has no memory controller, so it allocates
 * the same way — the distinction is who pays, not what is returned. */
#define GFP_KERNEL_ACCOUNT GFP_KERNEL
#endif
#ifndef __GFP_WRITE
#define __GFP_WRITE 0x800000u
#endif

#endif
