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


/* Nesting depth for lockdep, which is not here — so the depth is accepted and
 * discarded, and the lock behaves exactly as the un-nested call. */
#define mutex_lock_nested(m, subclass) do { (void)(subclass); mutex_lock(m); } while (0)
#define mutex_lock_interruptible_nested(m, subclass) \
	({ (void)(subclass); mutex_lock_interruptible(m); })
#define mutex_lock_nest_lock(m, nest) do { (void)(nest); mutex_lock(m); } while (0)


/* The nesting class a second acquisition of the same lock type declares. There
 * is no lockdep here to tell them apart, so it is accepted and discarded. */
#define SINGLE_DEPTH_NESTING 1


/* lockdep's per-lock map. There is no lockdep here, so it holds nothing — but
 * imported code names the member when it passes a lock's identity around, so it
 * has to exist. */
struct lockdep_map { int unused; };


/* The lockdep annotations a lock carries and the calls that would feed them.
 * Without lockdep the map holds nothing and the annotations do nothing — but
 * the member has to exist, because imported code passes &lock->dep_map around
 * as the lock's identity. */
/* The argument is NOT consumed, deliberately. Upstream's non-lockdep forms
 * expand to nothing at all, and the map they are handed is often a variable
 * that only exists under CONFIG_LOCKDEP — drm_connector.c has exactly one, and
 * a (void)(map) here made the DRM core stop compiling on an identifier that is
 * not supposed to exist. */
#define mutex_acquire(map, subclass, trylock, ip) do { } while (0)
#define mutex_release(map, ip)                    do { } while (0)
#define lock_acquire(map, subclass, trylock, read, check, nest, ip) \
	do { } while (0)
#define lock_release(map, ip) do { } while (0)


/* The raw initialiser, which upstream's mutex_init macro expands to. */
#define __mutex_init(m, name, key) do { (void)(name); (void)(key); mutex_init(m); } while (0)

#endif
