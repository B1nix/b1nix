/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DMA_RESV_H
#define LKPI_LINUX_DMA_RESV_H

#include <linux/dma-fence.h>
#include <linux/ww_mutex.h>
#include <lkpi/ww_mutex.h>

/*
 * A buffer object's reservation: the lock that guards it, plus the fences that
 * say when the GPU is finished with it.
 *
 * Two kinds of fence, and the distinction is the whole point. A WRITE fence
 * excludes everything — anyone reading or writing must wait for it. READ fences
 * do not exclude each other, so several engines can sample the same texture at
 * once, but a writer must wait for all of them. Collapsing the two into one
 * list still works and quietly serialises every read, which is a performance
 * bug that looks like correct code.
 *
 * The lock is a ww_mutex because a submission locks a whole set of these at
 * once and learns the set from userspace — see <lkpi/ww_mutex.h> for why that
 * needs wound/wait rather than ordering.
 */

enum dma_resv_usage {
	DMA_RESV_USAGE_KERNEL = 0, /* kernel-internal moves: excludes everything */
	DMA_RESV_USAGE_WRITE = 1,
	DMA_RESV_USAGE_READ = 2,
	DMA_RESV_USAGE_BOOKKEEP = 3,
};

struct dma_resv_fence {
	struct dma_fence *fence;
	enum dma_resv_usage usage;
};

struct dma_resv {
	struct ww_mutex lock;
	struct dma_resv_fence *fences;
	u32 count;
	u32 capacity;
};

void dma_resv_init(struct dma_resv *obj);
void dma_resv_fini(struct dma_resv *obj);

/* Make room for `num` more fences before taking any lock the caller cannot
 * allocate under. Returns 0 or -ENOMEM. */
int dma_resv_reserve_fences(struct dma_resv *obj, unsigned int num);

/* Add a fence. Replaces any earlier fence from the same context, so a buffer
 * used repeatedly by one engine keeps one fence rather than a growing list. */
void dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence,
                        enum dma_resv_usage usage);

/* 1 if every fence matching `usage` has signalled. */
int dma_resv_test_signaled(struct dma_resv *obj, enum dma_resv_usage usage);

/* Wait for them. `timeout_ticks` of 0 means poll. Returns >0 remaining ticks,
 * 0 on timeout. */
long dma_resv_wait_timeout(struct dma_resv *obj, enum dma_resv_usage usage,
                           int intr, unsigned long timeout_ticks);

/* Which fences a reader has to wait for: the writes, plus anything the kernel
 * moved. A reader does not wait for other readers, which is the distinction the
 * usage levels exist to express. */
static inline enum dma_resv_usage dma_resv_usage_rw(bool write)
{
	return write ? DMA_RESV_USAGE_READ : DMA_RESV_USAGE_WRITE;
}

/*
 * One fence standing for everything matching `usage`. With several, they would
 * have to be merged — and b1nix's dma_fence has no array form to merge into
 * (see dma_fence_unwrap_merge), so more than one is reported as unsupported
 * rather than as the first fence, which would claim the rest are covered.
 */
int dma_resv_get_singleton(struct dma_resv *obj, enum dma_resv_usage usage,
                           struct dma_fence **fence);

static inline int dma_resv_lock(struct dma_resv *obj,
                                struct ww_acquire_ctx *ctx)
{
	return ww_mutex_lock(&obj->lock, ctx);
}

static inline int dma_resv_lock_interruptible(struct dma_resv *obj,
                                              struct ww_acquire_ctx *ctx)
{
	/* b1nix kernel threads running the DRM core are not signal targets, so an
	 * interruptible acquire cannot actually be interrupted; it is the plain
	 * one. Callers that check for -EINTR simply never see it. */
	return ww_mutex_lock(&obj->lock, ctx);
}

static inline int dma_resv_lock_slow_interruptible(struct dma_resv *obj,
                                                   struct ww_acquire_ctx *ctx)
{
	ww_mutex_lock_slow(&obj->lock, ctx);
	return 0;
}

static inline void dma_resv_lock_slow(struct dma_resv *obj,
                                      struct ww_acquire_ctx *ctx)
{
	ww_mutex_lock_slow(&obj->lock, ctx);
}

static inline bool dma_resv_trylock(struct dma_resv *obj)
{
	return ww_mutex_trylock(&obj->lock, 0) != 0;
}

static inline void dma_resv_unlock(struct dma_resv *obj)
{
	ww_mutex_unlock(&obj->lock);
}

/* Assert the caller holds the reservation. Not checked — b1nix's lock checker
 * knows nothing about ww classes, see <linux/lockdep.h> — so this documents the
 * requirement rather than enforcing it. */
#define dma_resv_assert_held(obj) do { (void)(obj); } while (0)
#define dma_resv_held(obj)        (1)

static inline bool dma_resv_is_locked(struct dma_resv *obj)
{
	return obj->lock.locked != 0;
}

#endif
