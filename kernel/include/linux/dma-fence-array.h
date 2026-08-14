/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_DMA_FENCE_ARRAY_H
#define LKPI_LINUX_DMA_FENCE_ARRAY_H
#include <linux/dma-fence.h>
#include <linux/types.h>

/*
 * One fence standing for several: signalled when the last of them is.
 *
 * The core already has dma-fence and dma-fence-chain; this is the third shape,
 * and the one a driver builds when a submission waits on a set of dependencies
 * that have no order between them. The array owns a reference on each member and
 * drops them all when it is released.
 */
struct dma_fence_array {
	struct dma_fence base;
	spinlock_t lock;
	unsigned int num_fences;
	atomic_t num_pending;
	struct dma_fence **fences;
	/* One callback block for the whole array, not one per member: every member
	 * decrements the same counter, so they can share it — and dma_fence's
	 * callback list links through the block, so a member that is still pending
	 * when another signals keeps it alive. */
	struct dma_fence_cb cb_storage;
};

struct dma_fence_array *dma_fence_array_create(int num_fences,
                                               struct dma_fence **fences,
                                               u64 context, unsigned seqno,
                                               bool signal_on_any);
bool dma_fence_match_context(struct dma_fence *fence, u64 context);
struct dma_fence *dma_fence_array_first(struct dma_fence *head);
struct dma_fence *dma_fence_array_next(struct dma_fence *head, unsigned int index);
bool dma_fence_is_array(struct dma_fence *fence);
static inline struct dma_fence_array *to_dma_fence_array(struct dma_fence *fence)
{ return dma_fence_is_array(fence) ? (struct dma_fence_array *)fence : 0; }

/* Walk the fences an array holds, and any arrays nested inside it. The head
 * fence may not be an array at all, in which case the walk visits it alone —
 * which is what the callers rely on. */
#define dma_fence_array_for_each(fence, index, head)                     \
	for (index = 0, fence = dma_fence_array_first(head); fence;          \
	     ++(index), fence = dma_fence_array_next(head, index))

#endif
