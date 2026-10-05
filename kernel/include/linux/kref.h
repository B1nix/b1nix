/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KREF_H
#define LKPI_LINUX_KREF_H
#include <lkpi/kref.h>
#include <linux/kernel.h>
/* Linux's kref_put takes the release as a plain function pointer with the same
 * shape lkpi uses, so the forward is direct. */
#define kref_put(kref, release) kref_put((kref), (release))

/*
 * Release under a lock the caller names; `release` returns with it dropped.
 *
 * The last reference goes UNDER the lock, as upstream's refcount_dec_and_lock
 * does: the common put (not the last) pays nothing, and the final one cannot
 * race a lookup that takes a reference under the same lock. Decrementing to
 * zero first and locking after let i915's frontbuffer lookup take a reference
 * on an object whose release had already begun.
 */
#define kref_put_lock(kref, release, lock)                                  \
	({                                                                  \
		int __z = 0;                                                \
		if (!refcount_dec_not_one(&(kref)->refcount)) {             \
			spin_lock(lock);                                    \
			if (refcount_dec_and_test(&(kref)->refcount)) {     \
				release(kref);                              \
				__z = 1;                                    \
			} else {                                            \
				spin_unlock(lock);                          \
			}                                                   \
		}                                                           \
		__z;                                                        \
	})
#define kref_put_mutex(kref, release, mutex)                                \
	({                                                                  \
		int __z = 0;                                                \
		if (!refcount_dec_not_one(&(kref)->refcount)) {             \
			mutex_lock(mutex);                                  \
			if (refcount_dec_and_test(&(kref)->refcount)) {     \
				release(kref);                              \
				__z = 1;                                    \
			} else {                                            \
				mutex_unlock(mutex);                        \
			}                                                   \
		}                                                           \
		__z;                                                        \
	})

#endif
