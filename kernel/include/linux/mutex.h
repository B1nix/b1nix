/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_MUTEX_H
#define LKPI_LINUX_MUTEX_H

#include <linux/atomic.h>
#include <lkpi/lock.h>

/*
 * Sleeping mutex, onto lkpi's.
 *
 * A wrapper struct rather than `#define mutex lkpi_mutex`. That define looks
 * tempting and is actively destructive: `mutex` is also a *member name* in
 * imported structs (drm_plane::mutex, drm_modeset_lock::mutex), and a macro
 * rewrites those too, so the struct grows a field nobody references and every
 * use of the real one fails to compile. The lesson generalises — a macro over a
 * word this common cannot be scoped to the uses you meant.
 *
 * It parks the caller on a scheduler wait channel, so it MAY be held across a
 * sleep and must NOT be taken from an interrupt handler — the opposite of the
 * spinlock in <linux/spinlock.h>.
 */

struct mutex {
	struct lkpi_mutex lk;
};

static inline void mutex_init(struct mutex *m) { lkpi_mutex_init(&m->lk); }
static inline void mutex_lock(struct mutex *m) { lkpi_mutex_lock(&m->lk); }
static inline void mutex_unlock(struct mutex *m) { lkpi_mutex_unlock(&m->lk); }
static inline int mutex_trylock(struct mutex *m)
{
	return lkpi_mutex_trylock(&m->lk);
}
static inline int mutex_is_locked(struct mutex *m) { return m->lk.locked != 0; }
static inline int mutex_lock_interruptible(struct mutex *m)
{
	lkpi_mutex_lock(&m->lk);
	return 0;
}

/* lkpi's mutex owns no allocation, so there is nothing to tear down; the call
 * exists because imported code balances every init with one. */
static inline void mutex_destroy(struct mutex *m) { (void)m; }

/*
 * Drop a reference and take the lock only if it hit zero — atomically with
 * respect to anyone else doing the same. Doing it as two steps lets a second
 * caller see zero as well and both proceed to free.
 */
static inline int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	/* Take the lock first, then decrement under it: the lock is what makes
	 * the decision single, and the cost is holding it for one atomic. */
	mutex_lock(lock);
	if (atomic_dec_return(cnt) == 0)
		return 1;
	mutex_unlock(lock);
	return 0;
}

#define DEFINE_MUTEX(name) struct mutex name

#endif
