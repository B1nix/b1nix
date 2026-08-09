/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: fence chains and stubs.
 *
 * A chain link stands for its own fence and for every link before it, so one
 * wait covers a whole timeline. That is the property the syncobj timeline ioctl
 * is built on: userspace names a sequence number and gets a fence that signals
 * once everything up to it has.
 */

#include <linux/dma-fence-chain.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/errno.h>
#include <linux/slab.h>

/* ── stubs ──────────────────────────────────────────────────────── */

static struct dma_fence g_stub;
static volatile u32 g_stub_ready;

struct dma_fence *dma_fence_get_stub(void)
{
	/* One shared instance, so callers can compare a fence against it to ask
	 * "is this the nothing-to-wait-for fence". */
	if (!__atomic_load_n(&g_stub_ready, __ATOMIC_ACQUIRE)) {
		dma_fence_init_named(&g_stub, dma_fence_context_alloc(1), 1,
		                     "stub");
		dma_fence_signal(&g_stub);
		__atomic_store_n(&g_stub_ready, 1u, __ATOMIC_RELEASE);
	}
	return dma_fence_get(&g_stub);
}

struct dma_fence *dma_fence_allocate_private_stub(ktime_t timestamp)
{
	struct dma_fence *f = (struct dma_fence *)lkpi_kmalloc(
		sizeof(*f), GFP_KERNEL | __GFP_ZERO);
	if (!f)
		return 0;
	/* Private rather than shared: the caller will put it, and putting the
	 * shared stub would eventually free an object everyone else still holds. */
	dma_fence_init_named(f, dma_fence_context_alloc(1), 1, "private-stub");
	dma_fence_signal_timestamp(f, timestamp);
	return f;
}

/* ── chains ─────────────────────────────────────────────────────── */

struct dma_fence_chain *dma_fence_chain_alloc(void)
{
	return (struct dma_fence_chain *)lkpi_kmalloc(
		sizeof(struct dma_fence_chain), GFP_KERNEL | __GFP_ZERO);
}

void dma_fence_chain_free(struct dma_fence_chain *chain)
{
	lkpi_kfree(chain);
}

void dma_fence_chain_init(struct dma_fence_chain *chain, struct dma_fence *prev,
                          struct dma_fence *fence, u64 seqno)
{
	if (!chain)
		return;
	chain->prev = prev;
	chain->fence = fence;
	chain->prev_seqno = prev ? prev->seqno : 0;

	/* The chain shares its predecessor's context so the two are comparable as
	 * points on one timeline; a fresh context would make the sequence numbers
	 * meaningless against each other. */
	u64 context = prev ? prev->context : dma_fence_context_alloc(1);
	dma_fence_init_named(&chain->base, context, seqno, "chain");
}

struct dma_fence *dma_fence_chain_walk(struct dma_fence *fence)
{
	struct dma_fence_chain *chain = to_dma_fence_chain(fence);
	if (!chain)
		return 0;

	/* Walking drops the reference the iterator held on this link and takes one
	 * on the next, so a caller can stop anywhere without leaking. */
	struct dma_fence *prev = chain->prev ? dma_fence_get(chain->prev) : 0;
	dma_fence_put(fence);
	return prev;
}

int dma_fence_chain_find_seqno(struct dma_fence **pfence, u64 seqno)
{
	if (!pfence || !*pfence)
		return -EINVAL;
	if (seqno == 0)
		return 0;

	/* Walk back until the link that covers `seqno`. A sequence number beyond
	 * the head has not been submitted yet, and reporting success would hand
	 * the caller a fence that signals before the work exists. */
	struct dma_fence *fence = *pfence;
	if (fence->seqno < seqno)
		return -EINVAL;

	struct dma_fence_chain *chain = to_dma_fence_chain(fence);
	while (chain && chain->prev && chain->prev->seqno >= seqno) {
		fence = chain->prev;
		chain = to_dma_fence_chain(fence);
	}
	*pfence = dma_fence_get(fence);
	return 0;
}

struct dma_fence *dma_fence_unwrap_merge(struct dma_fence *f, ...)
{
	/*
	 * Merging several fences into one that signals when all have. b1nix's
	 * dma_fence is a single one-shot with no array form, so there is nothing to
	 * merge into; returning the first fence would claim the others are covered
	 * when they are not, so this reports absence and the caller waits on each.
	 */
	(void)f;
	return 0;
}
