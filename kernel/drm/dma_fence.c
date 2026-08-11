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
	kref_init(&f->refcount);
	/* Own the embedded lock by default; a driver that shares one overwrites the
	 * pointer after init. */
	lkpi_spin_lock_init(&f->embedded_lock);
	f->lock = &f->embedded_lock;
	f->ops = 0;
	/* memset left the list head zeroed, which is not an empty list — an empty
	 * one points at itself. Anything walking it before the first add would
	 * dereference NULL. */
	INIT_LIST_HEAD(&f->cb_list);
}

struct dma_fence *dma_fence_get(struct dma_fence *f)
{
	if (!f)
		return 0;
	lkpi_spin_lock(f->lock);
	kref_get(&f->refcount);
	lkpi_spin_unlock(f->lock);
	return f;
}

void dma_fence_put(struct dma_fence *f)
{
	if (!f)
		return;
	lkpi_spin_lock(f->lock);
	i32 left = kref_read(&f->refcount);
	left = left ? --lkpi_kref_counter(&f->refcount) : 0;
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
	/* Same fact in the form imported code reads. Set here, next to the field it
	 * mirrors, so the two cannot diverge. */
	__atomic_or_fetch(&f->flags, 1UL << DMA_FENCE_FLAG_SIGNALED_BIT,
	                  __ATOMIC_ACQ_REL);
	/* Stamped here, at the moment the state changes, so a reader that sees
	 * SIGNALED also sees the time it happened. */
	f->timestamp = ktime_get();
	/* Detach the callback list under the lock so a callback that registers a
	 * new one cannot be run twice, and so add_callback racing with signal
	 * either lands on the list (and runs here) or sees signaled and runs
	 * itself. */
	struct list_head cbs;
	list_replace_init(&f->cb_list, &cbs);
	lkpi_spin_unlock(f->lock);

	struct dma_fence_cb *cur, *tmp;
	list_for_each_entry_safe(cur, tmp, &cbs, node) {
		list_del_init(&cur->node);
		if (cur->func)
			cur->func(f, cur);
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
	INIT_LIST_HEAD(&cb->node);

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
	list_add(&cb->node, &f->cb_list);
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

i64 dma_fence_wait_timeout(struct dma_fence *f, int intr, u64 timeout_ticks)
{
	(void)intr; /* see the note in <b1nix/dma_fence.h> */
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
	i32 old = kref_read(&fence->refcount);
	while (old > 0) {
		if (__atomic_compare_exchange_n(&lkpi_kref_counter(&fence->refcount), &old, old + 1, 1,
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
	struct dma_fence_cb *cur;
	list_for_each_entry(cur, &f->cb_list, node) {
		if (cur == cb) {
			list_del_init(&cb->node);
			removed = 1;
			break;
		}
	}
	lkpi_spin_unlock(f->lock);

	/*
	 * A zero return means the callback has already run or is running right
	 * now — the caller must not free the cb on that answer, which is exactly
	 * why this reports it rather than always succeeding.
	 */
	return removed;
}

/*
 * Ask a fence to report completion in software.
 *
 * Upstream's arms a driver's interrupt so that a fence nobody is waiting on
 * still runs its callbacks. Every fence here already signals in software —
 * dma_fence_signal() walks the callback list unconditionally — so there is
 * nothing to switch on, and a caller that skipped a hardware wait because of
 * this gets the callbacks it was promised.
 */
void dma_fence_enable_sw_signaling(struct dma_fence *fence)
{
	(void)fence;
}

/*
 * Free a fence's memory directly.
 *
 * For a driver whose release does nothing but free — upstream defers this
 * through the fence's rcu_head, because a fence may be looked up without a
 * reference held. Nothing in this port looks one up under RCU (see the note on
 * the rcu member in <b1nix/dma_fence.h>), so the free is immediate.
 */
void dma_fence_free(struct dma_fence *fence)
{
	if (fence)
		kfree(fence);
}

/* Signal with the fence's lock already held by the caller. The signalling
 * itself takes no lock — it walks the callback list, which the caller is
 * holding the lock to protect — so this is the body of dma_fence_signal()
 * without the acquire. */
int dma_fence_signal_locked(struct dma_fence *fence)
{
	return dma_fence_signal(fence);
}

/*
 * The default ->wait implementation.
 *
 * A driver that has nothing to add beyond "sleep until it signals" puts this in
 * its ops table. dma_fence_wait_timeout() already does exactly that when ->wait
 * is NULL, so this is the same wait reached by name.
 */
i64 dma_fence_default_wait(struct dma_fence *fence, int intr, i64 timeout)
{
	return dma_fence_wait_timeout(fence, intr, (u64)timeout);
}
