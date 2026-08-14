/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: dma-resv, a buffer object's fence set.
 *
 * The list is flat and small on purpose. A buffer is touched by a handful of
 * engines, not thousands, so a linear scan beats any index — and it makes the
 * one rule that matters easy to state and easy to check: at most one fence per
 * (context, usage) pair, because a newer fence from the same context implies
 * every earlier one from it.
 *
 * Fences are refcounted here. The object holds a reference for as long as the
 * fence is in its list, which is what lets a caller drop its own reference
 * immediately after adding.
 */

#include <linux/errno.h>
#include <lkpi/env.h>
#include <lkpi/dma-mapping.h>
#include <lkpi/types.h>
#include <lkpi/ww_mutex.h>

/*
 * Deliberately built against <lkpi/ww_mutex.h> rather than <linux/dma-resv.h>:
 * the Linux header redefines ww_mutex_init and ww_acquire_init as two-argument
 * macros for imported callers, and this file is ours, so it uses the underlying
 * functions directly.
 */
#include <b1nix/dma_fence.h>

enum dma_resv_usage {
	DMA_RESV_USAGE_KERNEL = 0,
	DMA_RESV_USAGE_WRITE = 1,
	DMA_RESV_USAGE_READ = 2,
	DMA_RESV_USAGE_BOOKKEEP = 3,
};

struct dma_resv_fence {
	struct dma_fence *fence;
	enum dma_resv_usage usage;
};

struct dma_resv {
	struct ww_mutex lock;
	struct dma_resv_fence *fences;
	u32 count;
	u32 capacity;
};

void dma_resv_init(struct dma_resv *obj);
void dma_resv_fini(struct dma_resv *obj);
int dma_resv_reserve_fences(struct dma_resv *obj, unsigned int num);
void dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence,
                        enum dma_resv_usage usage);
int dma_resv_test_signaled(struct dma_resv *obj, enum dma_resv_usage usage);
long dma_resv_wait_timeout(struct dma_resv *obj, enum dma_resv_usage usage,
                           int intr, unsigned long timeout_ticks);

void dma_resv_init(struct dma_resv *obj)
{
	if (!obj)
		return;
	ww_mutex_init(&obj->lock);
	obj->fences = 0;
	obj->count = 0;
	obj->capacity = 0;
}

void dma_resv_fini(struct dma_resv *obj)
{
	if (!obj)
		return;
	for (u32 i = 0; i < obj->count; i++)
		dma_fence_put(obj->fences[i].fence);
	lkpi_kfree(obj->fences);
	obj->fences = 0;
	obj->count = 0;
	obj->capacity = 0;
}

int dma_resv_reserve_fences(struct dma_resv *obj, unsigned int num)
{
	if (!obj)
		return -EINVAL;
	u32 needed = obj->count + num;
	if (needed <= obj->capacity)
		return 0;

	/* Grown here, deliberately, rather than at add time: adding happens with
	 * the object locked and often from a path that cannot fail cleanly, so the
	 * allocation is made to happen first, where the caller can still back out.
	 * That is why Linux splits these two calls, and the split only helps if
	 * the growth really does happen here. */
	u32 capacity = obj->capacity ? obj->capacity * 2 : 4;
	if (capacity < needed)
		capacity = needed;

	struct dma_resv_fence *fences = (struct dma_resv_fence *)lkpi_kcalloc(
		capacity, sizeof(struct dma_resv_fence), GFP_KERNEL);
	if (!fences)
		return -ENOMEM;

	for (u32 i = 0; i < obj->count; i++)
		fences[i] = obj->fences[i];
	lkpi_kfree(obj->fences);
	obj->fences = fences;
	obj->capacity = capacity;
	return 0;
}

void dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence,
                        enum dma_resv_usage usage)
{
	if (!obj || !fence)
		return;

	/* Replace an earlier fence from the same context and usage: a later fence
	 * from one engine already implies everything that engine did before, so
	 * keeping both would grow the list without changing any answer. */
	for (u32 i = 0; i < obj->count; i++) {
		if (obj->fences[i].fence &&
		    obj->fences[i].fence->context == fence->context &&
		    obj->fences[i].usage == usage) {
			struct dma_fence *old = obj->fences[i].fence;
			obj->fences[i].fence = dma_fence_get(fence);
			dma_fence_put(old);
			return;
		}
	}

	if (obj->count == obj->capacity) {
		/* The caller should have reserved. Growing here anyway is better than
		 * dropping a fence — a dropped fence means waiting on this object
		 * returns before the GPU is done, which is silent corruption. */
		if (dma_resv_reserve_fences(obj, 1) != 0)
			return;
	}

	obj->fences[obj->count].fence = dma_fence_get(fence);
	obj->fences[obj->count].usage = usage;
	obj->count++;
}

/* Does a fence recorded with `have` have to be waited for by someone asking
 * about `want`? Usage is ordered by how much it excludes: KERNEL excludes
 * everything, WRITE excludes readers and writers, READ excludes only writers. */
static int usage_matches(enum dma_resv_usage have, enum dma_resv_usage want)
{
	return (u32)have <= (u32)want;
}

int dma_resv_test_signaled(struct dma_resv *obj, enum dma_resv_usage usage)
{
	if (!obj)
		return 1;
	for (u32 i = 0; i < obj->count; i++) {
		if (!usage_matches(obj->fences[i].usage, usage))
			continue;
		if (!dma_fence_is_signaled(obj->fences[i].fence))
			return 0;
	}
	return 1;
}

long dma_resv_wait_timeout(struct dma_resv *obj, enum dma_resv_usage usage,
                           int intr, unsigned long timeout_ticks)
{
	(void)intr;
	if (!obj)
		return timeout_ticks ? (long)timeout_ticks : 1;

	u64 start = lkpi_ticks();
	for (;;) {
		if (dma_resv_test_signaled(obj, usage)) {
			if (timeout_ticks == 0)
				return 1;
			u64 spent = lkpi_ticks() - start;
			return spent >= timeout_ticks ? 1
			                              : (long)(timeout_ticks - spent);
		}
		if (timeout_ticks == 0)
			return 0; /* poll: not signalled */
		if (lkpi_ticks() - start >= timeout_ticks) {
			/* Re-test once more before declaring a timeout: it may have
			 * signalled while we were deciding. */
			return dma_resv_test_signaled(obj, usage) ? 1 : 0;
		}
		if (lkpi_can_block())
			lkpi_sleep_ticks(1);
		else
			lkpi_cpu_relax();
	}
}

int dma_resv_get_singleton(struct dma_resv *obj, enum dma_resv_usage usage,
                           struct dma_fence **fence)
{
	if (!obj || !fence)
		return -EINVAL;

	struct dma_fence *found = 0;
	u32 matching = 0;
	for (u32 i = 0; i < obj->count; i++) {
		if (!usage_matches(obj->fences[i].usage, usage))
			continue;
		matching++;
		found = obj->fences[i].fence;
	}

	if (matching > 1) {
		/* Returning one of several would tell the caller the others are
		 * covered when they are not, and it would stop waiting too early. */
		*fence = 0;
		return -ENOSYS;
	}

	*fence = found ? dma_fence_get(found) : 0;
	return 0;
}

