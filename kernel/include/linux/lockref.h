/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_LOCKREF_H
#define LKPI_LINUX_LOCKREF_H

/*
 * <lkpi/lock.h>, not <linux/spinlock.h>.
 *
 * This header is reached from <linux/fs.h>, which <linux/types.h> reaches
 * through <linux/poll.h> — and <linux/spinlock.h> is what starts that chain, so
 * by the time we get here its include guard is set and its `spinlock_t` typedef
 * has not happened yet. The underlying type is the same one spinlock_t names,
 * so `spin_lock(&ref->lock)` in imported code still resolves — the operations
 * here call lkpi's directly for the same reason the type is spelled that way.
 */
#include <lkpi/lock.h>
#include <lkpi/kref.h>

/*
 * A reference count and the spinlock that guards it, in one word.
 *
 * Upstream's version tries a cmpxchg on the combined 64-bit word first and
 * falls back to taking the lock — which is what makes dentry reference counting
 * cheap enough to do on every path component. Here it always takes the lock:
 * the fast path is an optimisation, the slow path is the semantics, and getting
 * the semantics right first is the order that matters.
 *
 * The lock is a real one and is also taken directly by dentry code, so the
 * layout has to stay as upstream has it — `lock` first, `count` second.
 */
struct lockref {
	struct {
		struct lkpi_spinlock lock;
		int count;
	};
};

/* A dentry here starts at one and is freed at zero -- there is no unused
 * list -- so a get on zero or below is a get on freed memory. */
static inline void lockref_get(struct lockref *lockref)
{
	lkpi_spin_lock(&lockref->lock);
	if (__builtin_expect(lockref->count <= 0, 0))
		lkpi_refcount_bug("lockref_get", lockref, lockref->count);
	lockref->count++;
	lkpi_spin_unlock(&lockref->lock);
}

/* 1 if a reference was taken, 0 if the count was already zero — the object is
 * being freed and must not be resurrected. */
static inline int lockref_get_not_zero(struct lockref *lockref)
{
	int retval = 0;

	lkpi_spin_lock(&lockref->lock);
	if (lockref->count > 0) {
		lockref->count++;
		retval = 1;
	}
	lkpi_spin_unlock(&lockref->lock);
	return retval;
}

/* 1 if the count reached zero and the caller must now dispose of the object. */
static inline int lockref_put_or_lock(struct lockref *lockref)
{
	lkpi_spin_lock(&lockref->lock);
	if (__builtin_expect(lockref->count <= 0, 0))
		lkpi_refcount_bug("lockref_put_or_lock", lockref, lockref->count);
	if (lockref->count <= 1)
		return 0;   /* returns holding the lock, as upstream does */
	lockref->count--;
	lkpi_spin_unlock(&lockref->lock);
	return 1;
}

static inline int lockref_put_return(struct lockref *lockref)
{
	int count;

	lkpi_spin_lock(&lockref->lock);
	count = --lockref->count;
	lkpi_spin_unlock(&lockref->lock);
	/* Below zero is a second dput of a freed dentry, which dput would free
	 * again. */
	if (__builtin_expect(count < 0, 0))
		lkpi_refcount_bug("lockref_put_return", lockref, count + 1);
	return count;
}

static inline void lockref_mark_dead(struct lockref *lockref)
{
	lockref->count = -128;
}

static inline int __lockref_is_dead(const struct lockref *l)
{
	return l->count < 0;
}

#endif
