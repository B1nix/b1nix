/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_WW_MUTEX_H
#define LKPI_WW_MUTEX_H

#include <lkpi/types.h>

/*
 * ww_mutex — wound/wait mutexes, the lock class command submission is built on.
 *
 * The problem it solves. A submission names a set of buffer objects and must
 * hold all of their locks at once, but it learns the set from userspace, so two
 * submissions naming the same two buffers in opposite orders will deadlock under
 * ordinary mutexes. Sorting the set does not help: relocations can add a buffer
 * after locking has begun.
 *
 * The rule. Every attempt runs under an acquire context carrying a stamp from a
 * single monotonic counter, so any two contexts have a total order and the older
 * one (smaller stamp) has priority. When a context finds a lock held by a
 * *younger* context, it wounds that context: a flag is set on the holder. The
 * wounded context is not preempted — it keeps its locks and keeps running — but
 * the next time it asks for a lock it is told -EDEADLK and must release
 * everything it holds and start over. A context that meets an *older* holder
 * simply sleeps, because the older one is guaranteed to finish and never backs
 * off. Since only the youngest of any cycle ever backs off, no cycle survives
 * and the oldest attempt always makes progress: no deadlock and no livelock.
 *
 * The caller's obligation, and the reason this cannot be faked. On -EDEADLK the
 * caller MUST unlock every ww_mutex it holds under that context and then
 * re-acquire, starting with the contended lock through ww_mutex_lock_slow().
 * A shim that returns 0 unconditionally, or that maps this onto a plain mutex,
 * works in every single-threaded test and deadlocks under concurrent submission
 * — which is why this is implemented rather than stubbed.
 *
 * ww_mutex_lock() sleeps: never call it holding a spinlock.
 */

struct ww_acquire_ctx;

/*
 * Imported code passes `&lock->base` to lockdep as "the plain mutex inside".
 * There is no inner mutex here — the guard and the owner live in the ww_mutex
 * itself — so `base` is an empty marker that exists only to be addressable.
 * Giving it a real lock would create a second one nothing takes.
 */
struct ww_mutex_base {
	int unused;
};

struct ww_mutex {
	struct ww_mutex_base base;
	volatile int guard; /* a b1nix spinlock; opaque here, see <lkpi/lock.h> */
	volatile u32 locked;
	volatile usize owner;              /* task id + 1 of the holder, 0 = free */
	struct ww_acquire_ctx *volatile ctx; /* holder's context, may be NULL */
};

struct ww_acquire_ctx {
	u64 stamp;             /* age; smaller is older and wins */
	volatile u32 wounded;  /* an older context wants this one to back off */
	u32 acquired;          /* locks currently held under this context */
	u32 done;              /* ww_acquire_done was called */
	/* The lock this context is parked on, or NULL when it is running. A
	 * context wounded while it sleeps holds locks an older context is waiting
	 * behind, and nothing but a wake makes it re-test the flag and back off —
	 * so the wounder needs to know where to send that wake. */
	void *volatile parked_on;
};

/* Errors come back as negative b1nix errno values: -EDEADLK and -EALREADY,
 * both from <b1nix/errno.h> (pulled in via lkpi/types.h). */

void ww_mutex_init(struct ww_mutex *lock);

/* Start an attempt. Takes the next stamp, so contexts are ordered by the moment
 * they began rather than by anything the caller supplies. */
void ww_acquire_init(struct ww_acquire_ctx *ctx);

/* Declare that no further locks will be taken under this context. Purely a
 * statement of intent: it lets a later ww_mutex_lock be caught as a bug. */
void ww_acquire_done(struct ww_acquire_ctx *ctx);

/* End the attempt. All locks must already be released. */
void ww_acquire_fini(struct ww_acquire_ctx *ctx);

/*
 * Acquire `lock` under `ctx`.
 *
 *   0            — held.
 *   -EDEADLK     — back off: release every lock held under ctx, then re-acquire
 *                  starting with ww_mutex_lock_slow(lock, ctx).
 *   -EALREADY    — ctx already holds this lock; a caller bug, reported rather
 *                  than deadlocked on.
 *
 * `ctx` may be NULL for a lock taken outside any submission, in which case this
 * is an ordinary sleeping acquire that can neither wound nor be wounded.
 */
int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);

/*
 * Acquire the lock that caused the back-off. Cannot fail: the caller now holds
 * nothing, so it cannot be part of a cycle, and it waits however long it must.
 * Clears the wound left by the context that sent it here.
 */
void ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);

/* 1 if the lock was taken without sleeping. Never wounds anyone. */
int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);

void ww_mutex_unlock(struct ww_mutex *lock);

/* 1 when held by the calling task. */
int ww_mutex_is_locked_by_current(struct ww_mutex *lock);

/* Diagnostics: how many back-offs this kernel has issued, and how many wounds
 * were delivered. Used by the self-test to prove the path really ran. */
u64 ww_mutex_backoff_count(void);
u64 ww_mutex_wound_count(void);

#endif
