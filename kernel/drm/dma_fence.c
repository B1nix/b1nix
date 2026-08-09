/*
 * SPDX-License-Identifier: MIT
 *
 * M100 — dma-fence. See kernel/include/b1nix/dma_fence.h.
 */

#include <b1nix/console.h>
#include <b1nix/spinlock.h>
#include <b1nix/dma_fence.h>
#include <linux/ktime.h>

_Static_assert(sizeof(spinlock_t) == sizeof(int),
               "dma_fence's lock word must match b1nix's spinlock_t");
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

void dma_fence_init(struct dma_fence *f, const struct dma_fence_ops *ops,
                    struct lkpi_spinlock *lock, u64 context, u64 seqno)
{
	dma_fence_init_named(f, context, seqno, 0);
	if (!f)
		return;
	f->ops = ops;
	/* A caller sharing a lock passes it; otherwise the fence keeps its own. */
	if (lock)
		f->lock = lock;
}

void dma_fence_init_named(struct dma_fence *f, u64 context, u64 seqno,
                          const char *name)
{
	if (!f)
		return;
	memset(f, 0, sizeof(*f));
	f->context = context;
	f->seqno = seqno;
	f->name = name;
	f->refs = 1;
	/* Own the embedded lock by default; a driver that shares one overwrites the
	 * pointer after init. */
	lkpi_spin_lock_init(&f->embedded_lock);
	f->lock = &f->embedded_lock;
	f->ops = 0;
}

struct dma_fence *dma_fence_get(struct dma_fence *f)
{
	if (!f)
		return 0;
	lkpi_spin_lock(f->lock);
	f->refs++;
	lkpi_spin_unlock(f->lock);
	return f;
}

void dma_fence_put(struct dma_fence *f)
{
	if (!f)
		return;
	lkpi_spin_lock(f->lock);
	u32 left = f->refs ? --f->refs : 0;
	dma_fence_release_fn release = f->release;
	lkpi_spin_unlock(f->lock);
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

	lkpi_spin_lock(f->lock);
	if (f->signaled) {
		lkpi_spin_unlock(f->lock);
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
	lkpi_spin_unlock(f->lock);

	while (cbs) {
		struct dma_fence_cb *next = cbs->next;
		cbs->next = 0;
		if (cbs->func)
			cbs->func(f, cbs);
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
                           dma_fence_cb_fn func)
{
	return dma_fence_add_callback_data(f, cb, func, 0);
}

int dma_fence_add_callback_data(struct dma_fence *f, struct dma_fence_cb *cb,
                                dma_fence_cb_fn func, void *data)
{
	if (!f || !cb || !func)
		return -EINVAL;
	cb->func = func;
	cb->data = data;
	cb->next = 0;

	lkpi_spin_lock(f->lock);
	if (f->signaled) {
		lkpi_spin_unlock(f->lock);
		/* The callback takes the cb, not the data — the same signature the
		 * deferred path uses. Passing `data` here instead handed the callback
		 * the payload where it expected the cb, and its first dereference
		 * faulted. */
		func(f, cb);
		return -ENOENT;
	}
	cb->next = f->cbs;
	f->cbs = cb;
	lkpi_spin_unlock(f->lock);
	return 0;
}

int dma_fence_wait(struct dma_fence *f, int intr)
{
	/* Accepted and ignored: see the header. */
	(void)intr;
	return dma_fence_wait_uninterruptible(f);
}

int dma_fence_wait_uninterruptible(struct dma_fence *f)
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
		int rc = dma_fence_wait_uninterruptible(fences[i]);
		if (rc && !first_error)
			first_error = rc;
	}
	return first_error;
}

struct dma_fence *dma_fence_get_rcu(struct dma_fence *fence)
{
	if (!fence)
		return 0;
	/* Only take the reference if the fence is still alive. A plain increment
	 * would resurrect one whose count had already reached zero. */
	u32 old = __atomic_load_n(&fence->refs, __ATOMIC_ACQUIRE);
	while (old > 0) {
		if (__atomic_compare_exchange_n(&fence->refs, &old, old + 1, 1,
		                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return fence;
	}
	return 0;
}

struct dma_fence *dma_fence_get_rcu_safe(struct dma_fence **slot)
{
	if (!slot)
		return 0;
	for (;;) {
		struct dma_fence *fence =
			__atomic_load_n(slot, __ATOMIC_ACQUIRE);
		if (!fence)
			return 0;
		if (!dma_fence_get_rcu(fence))
			continue; /* it died between the load and the get; re-read */
		/*
		 * Confirm the slot still holds what we took a reference on. Without
		 * this a writer that swapped the pointer after our load leaves us
		 * holding a reference to a fence nobody else can see — the caller
		 * would wait on the wrong work and conclude the GPU was finished.
		 */
		if (fence == __atomic_load_n(slot, __ATOMIC_ACQUIRE))
			return fence;
		dma_fence_put(fence);
	}
}

void dma_fence_set_error(struct dma_fence *fence, int error)
{
	if (!fence)
		return;
	/* Setting an error after the fence signalled would change an answer a
	 * waiter has already acted on. */
	if (__atomic_load_n(&fence->signaled, __ATOMIC_ACQUIRE))
		return;
	fence->error = error;
}

int dma_fence_signal_timestamp(struct dma_fence *f, ktime_t timestamp)
{
	/* The timestamp is the caller's observation of when the hardware
	 * finished; taking one here would record when the kernel noticed, which
	 * is a different and less useful number. b1nix's fence carries no
	 * timestamp field yet, so it is accepted and dropped rather than replaced
	 * with a worse one. */
	(void)timestamp;
	return dma_fence_signal(f);
}

int dma_fence_remove_callback(struct dma_fence *f, struct dma_fence_cb *cb)
{
	if (!f || !cb)
		return 0;

	lkpi_spin_lock(f->lock);
	int removed = 0;
	struct dma_fence_cb **link = &f->cbs;
	while (*link) {
		if (*link == cb) {
			*link = cb->next;
			cb->next = 0;
			removed = 1;
			break;
		}
		link = &(*link)->next;
	}
	lkpi_spin_unlock(f->lock);

	/*
	 * A zero return means the callback has already run or is running right
	 * now — the caller must not free the cb on that answer, which is exactly
	 * why this reports it rather than always succeeding.
	 */
	return removed;
}
