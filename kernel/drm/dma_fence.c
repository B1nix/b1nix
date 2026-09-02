/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M100 — dma-fence. See kernel/include/b1nix/dma_fence.h.
 */

#include <lkpi/env.h>
#include <b1nix/arch.h>
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

static int fence_enable_signaling(struct dma_fence *f);

/* Where the notification chain stops.
 *
 * Four counts, because four things have to happen and the driver's own logs
 * distinguish none of them: the driver is asked to arm, it accepts, something
 * signals the fence, and the signal reaches the callbacks that wake waiters.
 * Read back through lkpi_fence_counts(). */
static u64 g_fence_arm_asked;
static u64 g_fence_arm_accepted;
static u64 g_fence_signalled;
static u64 g_fence_callbacks;

void lkpi_fence_counts(u64 *asked, u64 *accepted, u64 *signalled, u64 *callbacks)
{
	if (asked)     *asked = g_fence_arm_asked;
	if (accepted)  *accepted = g_fence_arm_accepted;
	if (signalled) *signalled = g_fence_signalled;
	if (callbacks) *callbacks = g_fence_callbacks;
}

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

/*
 * The flag is the truth, not the int beside it.
 *
 * Upstream keeps the signalled state in fence->flags, and imported code sets it
 * there directly: i915's breadcrumbs never call into the dma-fence API at all —
 * they test_and_set DMA_FENCE_FLAG_SIGNALED_BIT and run the callbacks
 * themselves, which is what makes an interrupt-driven fence cheap. This shim
 * kept its own `signaled` int as the source of truth, so a fence i915 had
 * signalled still read as pending: the callback ran, the waiter woke, re-checked
 * dma_fence_is_signaled(), was told no, and went back to sleep until its
 * timeout. Every GPU wait in the driver ended that way.
 *
 * Both are now read, and the int stays as a mirror for the paths that set it.
 */
/* What this fence has RECORDED about itself — no driver involved.
 *
 * The distinction matters where the state is changed rather than read: the
 * "already signalled?" guard in the signal path must consult only what has been
 * recorded, because a driver-backed fence answers "complete" from the hardware
 * the moment the work finishes, and a guard that believes it refuses to ever
 * perform the signal — leaving the flag unset, the callbacks unrun, and every
 * waiter parked. */
static int fence_signal_recorded(struct dma_fence *f)
{
	if (!f)
		return 1;
	if (__atomic_load_n(&f->flags, __ATOMIC_ACQUIRE) &
	    (1UL << DMA_FENCE_FLAG_SIGNALED_BIT))
		return 1;
	return f->signaled;
}

/* The state as anyone waiting on it should see it, driver included. Safe to
 * call with f->lock held. */
static int fence_signaled(struct dma_fence *f)
{
	if (!f)
		return 1;
	if (__atomic_load_n(&f->flags, __ATOMIC_ACQUIRE) &
	    (1UL << DMA_FENCE_FLAG_SIGNALED_BIT))
		return 1;
	if (f->signaled)
		return 1;
	/* And what the driver says. For i915 this reads the breadcrumb the engine
	 * wrote, so a request the hardware finished is complete here whether or
	 * not an interrupt has been processed yet. */
	if (f->ops && f->ops->signaled)
		return f->ops->signaled(f) ? 1 : 0;
	return 0;
}

/*
 * Ask, and settle it.
 *
 * Upstream's dma_fence_is_signaled() does two things: it consults the driver
 * through ops->signaled, and if the driver says yes it signals the fence there
 * and then, which runs the callbacks. This shim did neither — it read a flag of
 * its own — so a fence whose work the GPU had finished still read as pending.
 * Every wait loop in the imported tree is written around this call, i915's
 * included: it woke, asked, was told no, and slept again until its timeout.
 */
int dma_fence_is_signaled(struct dma_fence *f)
{
	if (!f)
		return 1;
	if (__atomic_load_n(&f->flags, __ATOMIC_ACQUIRE) &
	    (1UL << DMA_FENCE_FLAG_SIGNALED_BIT))
		return 1;
	if (!fence_signaled(f))
		return 0;
	dma_fence_signal(f);
	return 1;
}

int dma_fence_error(struct dma_fence *f)
{
	return f ? f->error : 0;
}

/*
 * `held` says the caller already owns f->lock — dma_fence_signal_locked() is
 * called from inside a section that took it, and taking it again is a
 * self-deadlock, not a nesting. The state change happens under the lock either
 * way; only the acquire is skipped.
 */
static int fence_signal_common(struct dma_fence *f, int error, int held)
{
	if (!f)
		return -EINVAL;

	if (!held)
		lkpi_spin_lock(f->lock);
	if (fence_signal_recorded(f)) {
		if (!held)
			lkpi_spin_unlock(f->lock);
		return -EINVAL;
	}
	f->error = error;
	f->signaled = 1;
	g_fence_signalled++;
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
	if (!held)
		lkpi_spin_unlock(f->lock);

	/*
	 * The callbacks run with the lock still held when the caller owned it,
	 * which is what upstream does too: it is the caller's lock and its
	 * business when to drop it. A callback that takes the same lock is a bug
	 * in the callback either way.
	 */
	struct dma_fence_cb *cur, *tmp;
	list_for_each_entry_safe(cur, tmp, &cbs, node) {
		list_del_init(&cur->node);
		if (cur->func) {
			g_fence_callbacks++;
			cur->func(f, cur);
		}
	}

	scheduler_wake_all(f);
	return 0;
}

int dma_fence_signal(struct dma_fence *f)
{
	return fence_signal_common(f, 0, 0);
}

int dma_fence_signal_error(struct dma_fence *f, int error)
{
	return fence_signal_common(f, error ? error : -EIO, 0);
}

int dma_fence_add_callback(struct dma_fence *f, struct dma_fence_cb *cb,
                           dma_fence_cb_fn func)
{
	return dma_fence_add_callback_data(f, cb, func, 0);
}

/*
 * Tell the driver somebody is waiting.
 *
 * A fence does not signal itself. Drivers arm whatever notifies them — for
 * i915, putting the request on the engine's breadcrumb list and unmasking the
 * user interrupt — only when the core asks, through ops->enable_signaling, and
 * the core asks the first time anyone waits on the fence or registers a
 * callback. This shim never asked, so nothing was ever armed: the GPU executed
 * the work, the interrupt arrived, the breadcrumb worker found an empty signal
 * list, and the waiter slept until its timeout with the request long since
 * complete.
 *
 * Called with f->lock held, as upstream calls it: i915's implementation adds
 * the request to a list the interrupt handler walks, and expects the fence to
 * be pinned for the duration. A driver that answers false means the fence is
 * already effectively done, and the caller signals it.
 *
 * Returns 1 when the fence is now armed (or already signalled), 0 when the
 * driver declined and the fence must be signalled instead.
 */



int dma_fence_add_callback_data(struct dma_fence *f, struct dma_fence_cb *cb,
                                dma_fence_cb_fn func, void *data)
{
	if (!f || !cb || !func)
		return -EINVAL;
	cb->func = func;
	cb->data = data;
	INIT_LIST_HEAD(&cb->node);

	lkpi_spin_lock(f->lock);
	if (fence_signaled(f)) {
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
	/*
	 * Arm with the fence lock DROPPED.
	 *
	 * Upstream arms under it, and i915's arming takes the engine's breadcrumb
	 * lock beneath — an order that wedged a CPU here within seconds. The
	 * callback is already on the list, so a fence that signals in this window
	 * runs it; the only thing the lock would add is atomicity between adding
	 * and arming, and the arming path is idempotent.
	 */
	if (!fence_enable_signaling(f))
		dma_fence_signal(f);
	return 0;
}

/* Arm the driver's notification, once. Returns 0 only when the driver declines
 * — which means the fence is already effectively done and must be signalled. */
static int fence_enable_signaling(struct dma_fence *f)
{
	unsigned long was;

	if (fence_signaled(f))
		return 1;
	was = __atomic_fetch_or(&f->flags, 1UL << DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT,
	                        __ATOMIC_ACQ_REL);
	if (was & (1UL << DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT))
		return 1;
	if (!f->ops || !f->ops->enable_signaling)
		return 1;
	g_fence_arm_asked++;
	if (f->ops->enable_signaling(f)) {
		g_fence_arm_accepted++;
		return 1;
	}
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
	/* Deliberately does NOT arm ops->enable_signaling.
	 *
	 * This wait is the shim's own, and its callers reach it holding locks the
	 * driver's arming path also takes — i915's takes the engine's breadcrumb
	 * lock — so asking here inverts an order the interrupt path relies on and
	 * wedges the CPU. The place to ask is where a callback is registered, which
	 * is the path a driver's own waits go through; this one polls the flag and
	 * needs nobody armed to make progress.
	 */
	while (!fence_signaled(f)) {
		if (!scheduler_can_block()) {
			/* Interrupt context or pre-scheduler boot: poll. The signaller is
			 * another CPU or an interrupt on this one, so this terminates. */
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}
		/* Two-phase wait: publish on the fence's channel, re-check, then park.
		 * A signal landing in between therefore cannot be missed. */
		scheduler_wait_prepare(f);
		if (fence_signaled(f))
			scheduler_wait_cancel();
		else
			scheduler_wait_commit();
	}
	return f->error;
}

/* The cycle counter, which advances whether or not interrupts are enabled.
 * x86_64 spells that the TSC; aarch64 spells it CNTVCT_EL0. */
static inline u64 lkpi_rdtsc(void)
{
#if defined(__x86_64__)
	u32 lo, hi;

	__asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
#elif defined(__aarch64__)
	u64 v;

	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
#else
	return 0;
#endif
}

/* Cycles that counter advances in one 10 ms scheduler tick, or 0 when there is
 * no usable rate. On aarch64 the counter has its own frequency (CNTFRQ_EL0)
 * and the CPU's calibrated kHz would be the wrong scale. */
static inline u64 lkpi_cycles_per_tick(void)
{
#if defined(__aarch64__)
	u64 hz;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
	return hz ? hz / 100ull : 0;
#else
	u32 khz = arch_cpu_khz();

	return khz ? (u64)khz * 10ull : 0;
#endif
}

i64 dma_fence_wait_timeout(struct dma_fence *f, int intr, u64 timeout_ticks)
{
	(void)intr; /* see the note in <b1nix/dma_fence.h> */
	if (!f)
		return -EINVAL;
	u64 deadline = scheduler_get_ticks() + timeout_ticks;
	/*
	 * A second deadline on the CPU's own clock, for the same reason as in
	 * wait_for_completion_timeout(): a caller that cannot block is a caller
	 * with interrupts disabled, and the scheduler tick cannot advance there —
	 * so a deadline counted in ticks is one this loop can never reach. The TSC
	 * runs regardless. Without it a fence that never signals spins the CPU for
	 * good, and one that signals late costs the whole timeout in a busy loop.
	 */
	u64 tsc_deadline = 0;
	{
		u64 per_tick = lkpi_cycles_per_tick();

		if (per_tick)
			tsc_deadline = lkpi_rdtsc() + timeout_ticks * per_tick;
	}
	for (;;) {
		if (fence_signaled(f))
			return f->error ? (i64)f->error : 1;
		u64 now = scheduler_get_ticks();
		if (now >= deadline)
			return 0;
		if (!scheduler_can_block()) {
			if (tsc_deadline && lkpi_rdtsc() >= tsc_deadline)
				return 0;
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare_timeout(f, deadline - now);
		if (fence_signaled(f))
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
	if (dma_fence_is_signaled(fence))
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

/* Signal with the fence's lock already held by the caller: the same state
 * change, without the acquire. Routing this through dma_fence_signal() was a
 * self-deadlock — i915_request_retire() holds the lock across it. */
int dma_fence_signal_locked(struct dma_fence *fence)
{
	return fence_signal_common(fence, 0, 1);
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
