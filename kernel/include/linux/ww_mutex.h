/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_WW_MUTEX_H
#define LKPI_LINUX_WW_MUTEX_H
#include <lkpi/ww_mutex.h>
#include <linux/types.h>
/* Linux threads an acquire "class" through these so lockdep can tell one
 * submission's locks from another's. b1nix has no lockdep integration for them,
 * and the ordering is decided by the stamp regardless, so the class is accepted
 * and ignored rather than pretended to be checked. */
struct ww_class { const char *name; };

/*
 * Imported code reaches into `lock->base` to hand the plain mutex underneath to
 * lockdep. lkpi's ww_mutex has no separate inner mutex — the guard and the
 * owner live in the ww_mutex itself — so `base` is the lock, and the
 * annotations that use it compile away (see <linux/lockdep.h> for why they are
 * not silently claimed to have checked anything).
 */
static inline int ww_mutex_is_locked(struct ww_mutex *lock)
{
	return lock && lock->locked != 0;
}
#define DEFINE_WW_CLASS(name) struct ww_class name = { #name }

/* The class every dma-resv lock is created under. One class for all of them is
 * right here: ordering is decided by the stamp, not by the class, so a second
 * class would distinguish nothing. */
extern struct ww_class reservation_ww_class;

static inline int ww_mutex_lock_interruptible(struct ww_mutex *lock,
                                              struct ww_acquire_ctx *ctx)
{
	/* Not actually interruptible here: b1nix kernel threads are not signal
	 * targets, so a caller checking for -EINTR never sees it. */
	return ww_mutex_lock(lock, ctx);
}

/* b1nix kernel threads are not signal targets, so an interruptible acquire
 * cannot actually be interrupted; callers checking for -EINTR never see it. */
static inline int ww_mutex_lock_slow_interruptible(struct ww_mutex *lock,
                                                   struct ww_acquire_ctx *ctx)
{
	ww_mutex_lock_slow(lock, ctx);
	return 0;
}
#define ww_mutex_init(lock, class) do { (void)(class); ww_mutex_init(lock); } while (0)
#define ww_acquire_init(ctx, class) do { (void)(class); ww_acquire_init(ctx); } while (0)
#endif
