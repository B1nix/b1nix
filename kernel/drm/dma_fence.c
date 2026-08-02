/*
 * SPDX-License-Identifier: MIT
 *
 * M100 — dma-fence. See kernel/include/b1nix/dma_fence.h.
 */

#include <b1nix/console.h>
#include <b1nix/dma_fence.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <string.h>

static u64 g_next_context = 1;
static spinlock_t g_context_lock = SPINLOCK_INIT;

u64 dma_fence_context_alloc(u64 count)
{
	if (count == 0)
		count = 1;
	u64 flags;
	spin_lock_irqsave(&g_context_lock, &flags);
	u64 base = g_next_context;
	g_next_context += count;
	spin_unlock_irqrestore(&g_context_lock, flags);
	return base;
}

void dma_fence_init(struct dma_fence *f, u64 context, u64 seqno,
                    const char *name)
{
	if (!f)
		return;
	memset(f, 0, sizeof(*f));
	f->context = context;
	f->seqno = seqno;
	f->name = name;
	f->refs = 1;
	f->lock = SPINLOCK_INIT;
}

struct dma_fence *dma_fence_get(struct dma_fence *f)
{
	if (!f)
		return 0;
	u64 flags;
	spin_lock_irqsave(&f->lock, &flags);
	f->refs++;
	spin_unlock_irqrestore(&f->lock, flags);
	return f;
}

void dma_fence_put(struct dma_fence *f)
{
	if (!f)
		return;
	u64 flags;
	spin_lock_irqsave(&f->lock, &flags);
	u32 left = f->refs ? --f->refs : 0;
	dma_fence_release_fn release = f->release;
	spin_unlock_irqrestore(&f->lock, flags);
	if (left == 0 && release)
		release(f);
}

int dma_fence_is_signaled(struct dma_fence *f)
{
	return f ? f->signaled : 1;
}

int dma_fence_error(struct dma_fence *f)
{
	return f ? f->error : 0;
}

static int fence_signal_common(struct dma_fence *f, int error)
{
	if (!f)
		return -EINVAL;

	u64 flags;
	spin_lock_irqsave(&f->lock, &flags);
	if (f->signaled) {
		spin_unlock_irqrestore(&f->lock, flags);
		return -EINVAL;
	}
	f->error = error;
	f->signaled = 1;
	/* Detach the callback list under the lock so a callback that registers a
	 * new one cannot be run twice, and so add_callback racing with signal
	 * either lands on the list (and runs here) or sees signaled and runs
	 * itself. */
	struct dma_fence_cb *cbs = f->cbs;
	f->cbs = 0;
	spin_unlock_irqrestore(&f->lock, flags);

	while (cbs) {
		struct dma_fence_cb *next = cbs->next;
		cbs->next = 0;
		if (cbs->func)
			cbs->func(f, cbs->data);
		cbs = next;
	}

	scheduler_wake_all(f);
	return 0;
}

int dma_fence_signal(struct dma_fence *f)
{
	return fence_signal_common(f, 0);
}

int dma_fence_signal_error(struct dma_fence *f, int error)
{
	return fence_signal_common(f, error ? error : -EIO);
}

int dma_fence_add_callback(struct dma_fence *f, struct dma_fence_cb *cb,
                           dma_fence_cb_fn func, void *data)
{
	if (!f || !cb || !func)
		return -EINVAL;
	cb->func = func;
	cb->data = data;
	cb->next = 0;

	u64 flags;
	spin_lock_irqsave(&f->lock, &flags);
	if (f->signaled) {
		spin_unlock_irqrestore(&f->lock, flags);
		func(f, data);
		return -ENOENT;
	}
	cb->next = f->cbs;
	f->cbs = cb;
	spin_unlock_irqrestore(&f->lock, flags);
	return 0;
}

int dma_fence_wait(struct dma_fence *f)
{
	if (!f)
		return -EINVAL;
	while (!f->signaled) {
		if (!scheduler_can_block()) {
			/* Interrupt context or pre-scheduler boot: poll. The signaller is
			 * another CPU or an interrupt on this one, so this terminates. */
			__asm__ volatile("pause");
			tlb_shootdown_poll();
			continue;
		}
		/* Two-phase wait: publish on the fence's channel, re-check, then park.
		 * A signal landing in between therefore cannot be missed. */
		scheduler_wait_prepare(f);
		if (f->signaled)
			scheduler_wait_cancel();
		else
			scheduler_wait_commit();
	}
	return f->error;
}

i64 dma_fence_wait_timeout(struct dma_fence *f, u64 timeout_ticks)
{
	if (!f)
		return -EINVAL;
	u64 deadline = scheduler_get_ticks() + timeout_ticks;
	for (;;) {
		if (f->signaled)
			return f->error ? (i64)f->error : 1;
		u64 now = scheduler_get_ticks();
		if (now >= deadline)
			return 0;
		if (!scheduler_can_block()) {
			__asm__ volatile("pause");
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare_timeout(f, deadline - now);
		if (f->signaled)
			scheduler_wait_cancel();
		else
			scheduler_wait_commit();
	}
}

int dma_fence_wait_all(struct dma_fence **fences, u32 count)
{
	if (!fences)
		return -EINVAL;
	int first_error = 0;
	for (u32 i = 0; i < count; i++) {
		if (!fences[i])
			continue;
		int rc = dma_fence_wait(fences[i]);
		if (rc && !first_error)
			first_error = rc;
	}
	return first_error;
}
