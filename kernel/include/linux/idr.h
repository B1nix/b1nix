/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_IDR_H
#define LKPI_LINUX_IDR_H
#include <lkpi/idr.h>
#include <linux/types.h>
#include <linux/spinlock.h>

/*
 * Linux's idr_alloc carries a gfp argument; lkpi's allocator never sleeps, so
 * there is nothing for it to select and it is dropped here rather than
 * threaded through and ignored deeper down.
 */
#define idr_alloc(idr, ptr, start, end, gfp) idr_alloc((idr), (ptr), (start), (end))

/* Replace what an id points at, returning what was there. */
void *idr_replace(struct idr *idr, void *ptr, u32 id);

/*
 * Walk every allocated id. Bounded by the highest id ever handed out rather
 * than by the count: ids are not dense — freeing one leaves a hole — so
 * stopping after `count` entries would miss everything past the first gap.
 */
u32 idr_max_allocated(struct idr *idr);

#define idr_for_each_entry(idr, entry, id)                             \
	for ((id) = 0; (id) <= idr_max_allocated(idr); (id)++)             \
		if (((entry) = idr_find((idr), (id))) != 0)

/*
 * ida — an id allocator with no object attached, for numbering things whose
 * identity is the number itself (a connector index, a minor). lkpi's idr stores
 * a pointer per id; an ida is that with the pointer unused, so it is built on
 * the same allocator rather than a second one that could drift from it.
 */
#define DEFINE_IDA(name) struct ida name

/* Allocate an id no lower than `min`. */
static inline int ida_alloc_min(struct ida *ida, unsigned int min, gfp_t gfp)
{
	return ida_alloc_range(ida, min, 0, gfp);
}

/*
 * Linux's idr_preload reserves memory so the following idr_alloc can run under
 * a lock without allocating. lkpi's idr allocates on demand and never sleeps,
 * so there is nothing to preload and the pair is a no-op — the allocation still
 * happens, just at the point the caller thought it would not.
 */
static inline int idr_is_empty(struct idr *idr) { return idr_count(idr) == 0; }

#define idr_preload(gfp) do { (void)(gfp); } while (0)
#define idr_preload_end() do { } while (0)

/* struct ida and its operations live in <lkpi/idr.h>, so implementation files
 * on our side of the boundary can use them without the macros below. */


/* The pre-2019 ida spelling, still used by parts of i915. Same allocator; the
 * end is exclusive here as it was there, and 0 means "no upper bound". */
#define ida_simple_get(ida, start, end, gfp) \
	ida_alloc_range(ida, start, (end) ? (end) - 1 : ~0u, gfp)
#define ida_simple_remove(ida, id) ida_free(ida, id)

#endif
