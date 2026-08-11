/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_KREF_H
#define LKPI_LINUX_KREF_H
#include <lkpi/kref.h>
#include <linux/kernel.h>
/* Linux's kref_put takes the release as a plain function pointer with the same
 * shape lkpi uses, so the forward is direct. */
#define kref_put(kref, release) kref_put((kref), (release))

/* Release under a lock the caller names, taken only when the count reaches
 * zero. The point is the same as atomic_dec_and_lock's: the common put pays
 * nothing, and only the last one serialises. */
#define kref_put_lock(kref, release, lock)                        \
	({                                                            \
		int __z = refcount_dec_and_test(&(kref)->refcount);        \
		if (__z) { spin_lock(lock); release(kref); }               \
		__z;                                                      \
	})
#define kref_put_mutex(kref, release, mutex)                      \
	({                                                            \
		int __z = refcount_dec_and_test(&(kref)->refcount);        \
		if (__z) { mutex_lock(mutex); release(kref); }             \
		__z;                                                      \
	})

#endif
