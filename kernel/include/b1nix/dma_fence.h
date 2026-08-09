/* SPDX-License-Identifier: MIT */
#ifndef B1NIX_DMA_FENCE_H
#define B1NIX_DMA_FENCE_H

#include <b1nix/types.h>
#include <lkpi/lock.h>

/*
 * M100 — dma-fence.
 *
 * A dma-fence is a one-shot "this GPU work has finished" object: created
 * unsignalled, signalled exactly once, never reset. Everything else in a modern
 * graphics stack is built on that single guarantee — the scheduler waits on
 * dependencies, userspace waits on submission, and a display flip waits on the
 * render that fills it.
 *
 * Why b1nix needs it: virtio_gpu currently blocks the submitting task inside a
 * TSC-bounded busy-spin on the virtqueue used index. That burns a CPU, cannot
 * express "this job depends on that one", and cannot be waited on by anything
 * but its own caller. A fence replaces the spin with a scheduler wait and makes
 * completion an object other code can hold.
 *
 * Concurrency: dma_fence_signal() is safe from an interrupt handler.
 * dma_fence_wait() sleeps and must not be called under a spinlock.
 * Callbacks run in the signaller's context, so they must be short and must not
 * sleep — exactly the rule b1nix already applies to IRQ handlers.
 *
 * Lifetime: fences are refcounted. dma_fence_init() starts the count at one for
 * the creator; every additional holder takes dma_fence_get(). The `release`
 * callback runs when the last reference goes, which is how an embedding object
 * (a scheduler job) frees itself.
 */

struct dma_fence;

struct dma_fence_cb;

/*
 * Linux's callback signature: the cb itself, not a data pointer. Callers embed
 * the cb in their own struct and recover it with container_of, which is why a
 * separate data pointer is not needed — dma_fence_add_callback_data still keeps
 * one for b1nix's own callers, reachable through the cb.
 */
typedef void (*dma_fence_cb_fn)(struct dma_fence *fence,
                                struct dma_fence_cb *cb);
typedef void (*dma_fence_release_fn)(struct dma_fence *fence);

struct dma_fence_cb {
	dma_fence_cb_fn func;
	void *data;
	struct dma_fence_cb *next;
};

struct dma_fence_ops;

struct dma_fence {
	u64 context;          /* timeline this fence belongs to */
	u64 seqno;            /* position on that timeline */
	volatile int signaled;
	int error;            /* 0, or a negative errno set by signal_error */
	volatile u32 refs;
	const char *name;
	struct dma_fence_cb *cbs;
	dma_fence_release_fn release;
	/*
	 * A *pointer* to the lock, not the lock itself — the shape imported DRM
	 * code expects, because it takes the lock through <linux/spinlock.h> and
	 * recovers the owning object with container_of on the pointer. A fence
	 * that owns its lock points here at `embedded_lock`; one that shares a
	 * driver's lock points at that instead, which is the case the pointer
	 * exists for.
	 *
	 * It is an lkpi_spinlock rather than a b1nix one so this header carries no
	 * b1nix lock declarations into a translation unit compiling imported
	 * source. See <lkpi/env.h>.
	 */
	struct lkpi_spinlock *lock;
	struct lkpi_spinlock embedded_lock;

	/*
	 * Driver callbacks. Imported code reads fence->ops->get_driver_name and
	 * friends; b1nix's own fences leave it NULL and the accessors fall back to
	 * the fence's `name`.
	 */
	const struct dma_fence_ops *ops;
};

/*
 * What a driver supplies for its own fences. Only the entries the DRM core
 * actually calls; a NULL table is valid and means "use the defaults".
 */
struct dma_fence_ops {
	const char *(*get_driver_name)(struct dma_fence *fence);
	const char *(*get_timeline_name)(struct dma_fence *fence);
	/* bool, matching what imported drivers write. */
	_Bool (*enable_signaling)(struct dma_fence *fence);
	_Bool (*signaled)(struct dma_fence *fence);
	void (*release)(struct dma_fence *fence);
};

/* Allocate `count` fresh timeline contexts. Contexts are never reused, so two
 * fences from different timelines are always distinguishable. */
u64 dma_fence_context_alloc(u64 count);

/*
 * Initialise a fence on (context, seqno) with a refcount of one.
 *
 * The argument order is Linux's, because imported drivers call this directly.
 * `lock` may be NULL, in which case the fence uses its own embedded lock; a
 * driver that passes one is sharing a lock it already holds elsewhere, which
 * is the case the pointer exists for. `ops` may be NULL, and then the name
 * accessors fall back to `name`.
 */
void dma_fence_init(struct dma_fence *f, const struct dma_fence_ops *ops,
                    struct lkpi_spinlock *lock, u64 context, u64 seqno);

/* b1nix's own fences, which have no ops table, name themselves instead. */
void dma_fence_init_named(struct dma_fence *f, u64 context, u64 seqno,
                          const char *name);

struct dma_fence *dma_fence_get(struct dma_fence *f);
void dma_fence_put(struct dma_fence *f);

/* Signal the fence, run its callbacks and wake its waiters. Returns 0, or
 * -EINVAL when the fence was already signalled (signalling twice is a driver
 * bug, and silently accepting it hides the bug). */
int dma_fence_signal(struct dma_fence *f);
/* As above, recording an error that dma_fence_wait() then returns. */
int dma_fence_signal_error(struct dma_fence *f, int error);

int dma_fence_is_signaled(struct dma_fence *f);
/* The error recorded at signal time (0 when none, or when unsignalled). */
int dma_fence_error(struct dma_fence *f);

/* Sleep until signalled. Returns 0, or the fence's error. */
/*
 * Wait until the fence signals. `intr` asks for an interruptible wait; b1nix
 * kernel threads are not signal targets, so it is accepted and the wait is
 * uninterruptible either way — a caller checking for -ERESTARTSYS never sees
 * it. The argument is kept because imported code passes it and dropping it
 * would silently change which overload a call resolves to.
 */
int dma_fence_wait(struct dma_fence *f, int intr);

/* b1nix's own callers, which have no notion of interruptibility. */
int dma_fence_wait_uninterruptible(struct dma_fence *f);
/* Sleep until signalled or `timeout_ticks` scheduler ticks elapse. Returns the
 * remaining ticks (>0) on success, 0 on timeout, or the fence's negative error. */
i64 dma_fence_wait_timeout(struct dma_fence *f, u64 timeout_ticks);

/* Register `cb` to run when the fence signals. If it is already signalled the
 * callback runs immediately and -ENOENT is returned; otherwise 0. The caller
 * owns the cb storage until it fires. */
int dma_fence_add_callback(struct dma_fence *f, struct dma_fence_cb *cb,
                           dma_fence_cb_fn func);

/* The b1nix form, which also stashes a data pointer in the cb. Imported code
 * uses the three-argument form above and recovers its context with
 * container_of on the cb instead — which is why the data pointer is optional. */
int dma_fence_add_callback_data(struct dma_fence *f, struct dma_fence_cb *cb,
                                dma_fence_cb_fn func, void *data);

/* Wait for every fence in the array. Returns 0 or the first error. */
int dma_fence_wait_all(struct dma_fence **fences, u32 count);

/* M100 in-kernel self-test. Emits M100-SMOKE markers; no-op outside
 * b1nix.test=1. */
void dma_fence_selftest(void);

#endif
