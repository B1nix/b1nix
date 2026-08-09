/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DMA_FENCE_UNWRAP_H
#define LKPI_LINUX_DMA_FENCE_UNWRAP_H
#include <linux/dma-fence-chain.h>
/* Iterating the individual fences inside a chain or array. Same status as the
 * chain itself: declared, walked when the ioctls that build them exist. */
struct dma_fence_unwrap {
	struct dma_fence *chain;
	struct dma_fence *array;
	unsigned int index;
};

/* Merge a set of fences into one that signals when all of them have. Declared
 * with the rest of the chain surface; built when the ioctls that produce
 * multi-fence waits land. */
struct dma_fence *dma_fence_unwrap_merge(struct dma_fence *f, ...);
#endif
