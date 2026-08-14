/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_DMA_FENCE_CHAIN_H
#define LKPI_LINUX_DMA_FENCE_CHAIN_H
#include <linux/dma-fence.h>
#include <linux/kernel.h>
/* A fence that also stands for every fence before it in a timeline, so one wait
 * covers a whole sequence. Declared for the timeline-syncobj paths; the chain
 * walk lands with the ioctl that creates one. */
struct dma_fence_chain {
	struct dma_fence base;
	struct dma_fence *prev;
	struct dma_fence *fence;
	u64 prev_seqno;
};
/* Recover the chain from the fence embedded in it, or NULL if this fence is
 * not a chain link. Distinguishing them is the caller's job on Linux too — the
 * ops table is what tells them apart. */
static inline struct dma_fence_chain *to_dma_fence_chain(struct dma_fence *f)
{
	return f ? container_of(f, struct dma_fence_chain, base) : 0;
}

struct dma_fence_chain *dma_fence_chain_alloc(void);
void dma_fence_chain_free(struct dma_fence_chain *chain);
struct dma_fence *dma_fence_chain_walk(struct dma_fence *fence);
/* Walk a chain from `head` down. The body runs once per link; the iterator
 * takes and drops a reference on each, so the body may keep one by taking its
 * own. */
#define dma_fence_chain_for_each(iter, head) \
	for (iter = dma_fence_get(head); iter; iter = dma_fence_chain_walk(iter))

void dma_fence_chain_init(struct dma_fence_chain *chain, struct dma_fence *prev,
                          struct dma_fence *fence, u64 seqno);
/* Find the fence in a chain that covers `seqno`. Declared with the rest of the
 * timeline syncobj surface; walked when the ioctls that build chains land. */
int dma_fence_chain_find_seqno(struct dma_fence **pfence, u64 seqno);

/* The ops a chain node carries. Named because imported code compares a fence's
 * ops against them to recognise a chain. */
extern const struct dma_fence_ops dma_fence_chain_ops;

#endif
