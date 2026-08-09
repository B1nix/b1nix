/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DMA_FENCE_H
#define LKPI_LINUX_DMA_FENCE_H
#include <b1nix/dma_fence.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
/* Onto b1nix's own dma-fence (M100): refcounted one-shot completion with
 * callbacks, errors and timed waits. The names and shape already match, so
 * this is the include the imported source asks for rather than a translation. */
/*
 * Take a reference on a fence read from a location another CPU may be
 * replacing. The plain get would be a use-after-free: the fence can reach zero
 * between the load and the increment, so the reference has to be taken only if
 * the count is still non-zero, and the pointer re-read to confirm nothing
 * swapped it in the meantime.
 */
/* An already-signalled fence, shared by everyone who needs "nothing to wait
 * for" as a fence object. One instance, so callers can compare against it. */
/* Take a callback back off a fence that has not signalled. Returns 1 if it was
 * still registered — a 0 means the callback has already run or is running, and
 * the caller must not free the cb until it has. */
int dma_fence_remove_callback(struct dma_fence *fence, struct dma_fence_cb *cb);

/* Tell a fence's producer when the result is actually needed, so a GPU running
 * a power-saving clock can decide to hurry. b1nix's fences carry no deadline
 * and nothing acts on one, so this records nothing — a missed hint costs
 * latency, never correctness. */
static inline void dma_fence_set_deadline(struct dma_fence *fence,
                                          ktime_t deadline)
{
	(void)fence;
	(void)deadline;
}

struct dma_fence *dma_fence_get_stub(void);
/* A private already-signalled fence, allocated per caller rather than shared —
 * used where the caller needs one it can put without affecting anyone else. */
struct dma_fence *dma_fence_allocate_private_stub(ktime_t timestamp);

struct dma_fence *dma_fence_get_rcu_safe(struct dma_fence **slot);
struct dma_fence *dma_fence_get_rcu(struct dma_fence *fence);

/* Record an error on a fence that has not signalled yet. Setting one after it
 * has signalled would change an answer a waiter already acted on. */
void dma_fence_set_error(struct dma_fence *fence, int error);

/* Signal a fence and record when it happened. The timestamp is the caller's,
 * not one taken here: the moment that matters is when the hardware finished,
 * which the caller observed and this function did not. */
int dma_fence_signal_timestamp(struct dma_fence *fence, ktime_t timestamp);

#endif
