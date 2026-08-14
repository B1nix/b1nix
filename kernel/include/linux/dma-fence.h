/* SPDX-License-Identifier: GPL-2.0-only */
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


/*
 * Marking a critical section that must reach a signal.
 *
 * Upstream uses these to let lockdep prove that nothing inside the section can
 * block on something that is itself waiting for this fence — the deadlock that
 * takes a GPU down and is almost impossible to reproduce. There is no lockdep
 * here, so nothing is proved; the section is still marked so the annotation
 * survives a rebase and starts working the day lockdep does.
 */
static inline bool dma_fence_begin_signalling(void) { return true; }
static inline void dma_fence_end_signalling(bool cookie) { (void)cookie; }


/* Is fence f1 later in f2's timeline? Only meaningful for two fences on the
 * same context; the wrap-safe comparison is on the seqno. */
static inline bool dma_fence_is_later(struct dma_fence *f1, struct dma_fence *f2)
{
	if (f1->context != f2->context)
		return false;
	return (s64)(f1->seqno - f2->seqno) > 0;
}

/* Free a fence's memory directly, for a driver whose release does nothing else.
 * The kfree upstream reaches through the rcu head; there is no deferred free
 * here — a fence is not looked up under RCU in this port. */
void dma_fence_free(struct dma_fence *fence);

/* Signal with the fence's lock already held by the caller. */
int dma_fence_signal_locked(struct dma_fence *fence);

/* Ask a fence to start reporting completion in software. A driver that only
 * arms its interrupt when someone is waiting needs this before a wait that does
 * not go through dma_fence_wait(). */
void dma_fence_enable_sw_signaling(struct dma_fence *fence);

/* The default ->wait implementation, for a driver that wants the generic
 * sleep-on-signal behaviour in its ops table. */
i64 dma_fence_default_wait(struct dma_fence *fence, int intr, i64 timeout);

#endif
