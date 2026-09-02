/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M101 linuxkpi: wound/wait mutexes.
 *
 * See <lkpi/ww_mutex.h> for what the algorithm is and why command submission
 * needs it. This file is the mechanism: a stamp counter, a wound flag, and a
 * parking loop that checks the flag before it sleeps.
 *
 * The one subtlety worth stating here. A wounded context is not interrupted —
 * b1nix has no way to yank a lock back out of a running task, and doing so would
 * corrupt whatever the holder was in the middle of. The wound is therefore
 * advisory and is collected at the next acquire: either the holder asks for
 * another lock and is told -EDEADLK, or it finishes and releases everything,
 * which is the outcome the wounder wanted anyway. That is why a wounded context
 * that never takes another lock is not a bug — it simply completes.
 */

#include <b1nix/arch.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/tlb.h>
#include <lkpi/ww_mutex.h>

/* Stamp source. Monotonic across the whole kernel, so any two contexts are
 * comparable no matter which driver created them. Starts at 1 so 0 is never a
 * valid stamp. */
static volatile u64 g_ww_stamp_next = 1;

/* Diagnostics. Counted here rather than derived by the test, so the markers
 * report what the layer actually did. */
static volatile u64 g_ww_backoffs;
static volatile u64 g_ww_wounds;

static usize ww_current_id(void)
{
	struct task *t = current_task;
	return t ? t->id + 1 : 0;
}

void ww_mutex_init(struct ww_mutex *lock)
{
	if (!lock)
		return;
	lock->guard = SPINLOCK_INIT;
	lock->locked = 0;
	lock->owner = 0;
	lock->ctx = 0;
}

void ww_acquire_init(struct ww_acquire_ctx *ctx)
{
	if (!ctx)
		return;
	ctx->stamp = __atomic_fetch_add(&g_ww_stamp_next, 1ull, __ATOMIC_ACQ_REL);
	ctx->wounded = 0;
	ctx->acquired = 0;
	ctx->done = 0;
}

void ww_acquire_done(struct ww_acquire_ctx *ctx)
{
	if (ctx)
		ctx->done = 1;
}

void ww_acquire_fini(struct ww_acquire_ctx *ctx)
{
	if (!ctx)
		return;
	/* Releasing the context while it still holds locks would strand them: the
	 * stamp that ordered them is gone, so nothing can ever wound the holder
	 * again. Report it rather than leaving a lock nobody can reason about. */
	if (ctx->acquired != 0)
		panic("ww_acquire_fini with locks still held");
	ctx->stamp = 0;
	ctx->wounded = 0;
	ctx->done = 0;
}

/* Take the lock if free. Caller holds `guard`. */
static int ww_take_locked(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (lock->locked)
		return 0;
	lock->locked = 1;
	lock->owner = ww_current_id();
	lock->ctx = ctx;
	if (ctx)
		ctx->acquired++;
	return 1;
}

int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (!lock)
		return 0;
	u64 flags;
	spin_lock_irqsave((spinlock_t *)&lock->guard, &flags);
	int got = ww_take_locked(lock, ctx);
	spin_unlock_irqrestore((spinlock_t *)&lock->guard, flags);
	return got;
}

int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (!lock)
		return -EINVAL;
	if (ctx && ctx->done)
		panic("ww_mutex_lock after ww_acquire_done");

	for (;;) {
		u64 flags;
		spin_lock_irqsave((spinlock_t *)&lock->guard, &flags);

		if (ww_take_locked(lock, ctx)) {
			spin_unlock_irqrestore((spinlock_t *)&lock->guard, flags);
			return 0;
		}

		/* Already ours under this context: the caller listed the same buffer
		 * twice. Recursing would deadlock, so name the bug instead. */
		if (ctx && lock->ctx == ctx) {
			spin_unlock_irqrestore((spinlock_t *)&lock->guard, flags);
			return -EALREADY;
		}

		/* Wound/wait. We are older than the holder exactly when our stamp is
		 * smaller. Wound it: it keeps running and keeps its locks, but the next
		 * lock it asks for will be refused, which frees this one. */
		struct ww_acquire_ctx *holder = lock->ctx;
		if (ctx && holder && holder != ctx && ctx->stamp < holder->stamp &&
		    !holder->wounded) {
			holder->wounded = 1;
			__atomic_fetch_add(&g_ww_wounds, 1ull, __ATOMIC_RELAXED);
		}
		spin_unlock_irqrestore((spinlock_t *)&lock->guard, flags);

		/* Someone older wounded us while we were getting here. Back off before
		 * parking — sleeping now is exactly the cycle we are avoiding. */
		if (ctx && ctx->wounded) {
			__atomic_fetch_add(&g_ww_backoffs, 1ull, __ATOMIC_RELAXED);
			return -EDEADLK;
		}

		if (!scheduler_can_block()) {
			/* Cannot park (early boot or interrupt context). Spinning is wrong
			 * for a submission path but right here: the alternative is wedging
			 * the CPU, and TLB shootdowns still have to be serviced. */
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}

		/* Two-phase park, same as the plain mutex: publish, re-test under the
		 * guard, and only sleep if the lock is still held — otherwise a release
		 * landing in this window would be lost. The wound flag is re-tested too,
		 * so a wound delivered here is collected on the next pass instead of
		 * being slept through. */
		scheduler_wait_prepare(lock);
		spin_lock_irqsave((spinlock_t *)&lock->guard, &flags);
		int still_held = lock->locked;
		spin_unlock_irqrestore((spinlock_t *)&lock->guard, flags);
		if (still_held && !(ctx && ctx->wounded))
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

void ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (!lock)
		return;
	/* The caller has released everything, so it holds no lock anyone could be
	 * waiting behind and cannot be part of a cycle. Clearing the wound is what
	 * makes the retry able to sleep instead of bouncing off -EDEADLK forever. */
	if (ctx)
		ctx->wounded = 0;

	for (;;) {
		u64 flags;
		spin_lock_irqsave((spinlock_t *)&lock->guard, &flags);
		int got = ww_take_locked(lock, ctx);
		spin_unlock_irqrestore((spinlock_t *)&lock->guard, flags);
		if (got)
			return;

		if (!scheduler_can_block()) {
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare(lock);
		spin_lock_irqsave((spinlock_t *)&lock->guard, &flags);
		int still_held = lock->locked;
		spin_unlock_irqrestore((spinlock_t *)&lock->guard, flags);
		if (still_held)
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

void ww_mutex_unlock(struct ww_mutex *lock)
{
	if (!lock)
		return;
	u64 flags;
	spin_lock_irqsave((spinlock_t *)&lock->guard, &flags);
	struct ww_acquire_ctx *ctx = lock->ctx;
	if (ctx && ctx->acquired > 0)
		ctx->acquired--;
	lock->locked = 0;
	lock->owner = 0;
	lock->ctx = 0;
	spin_unlock_irqrestore((spinlock_t *)&lock->guard, flags);
	scheduler_wake_all(lock);
}

int ww_mutex_is_locked_by_current(struct ww_mutex *lock)
{
	if (!lock)
		return 0;
	return lock->locked && lock->owner == ww_current_id();
}

u64 ww_mutex_backoff_count(void)
{
	return __atomic_load_n(&g_ww_backoffs, __ATOMIC_ACQUIRE);
}

u64 ww_mutex_wound_count(void)
{
	return __atomic_load_n(&g_ww_wounds, __ATOMIC_ACQUIRE);
}
