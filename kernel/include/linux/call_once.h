/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CALL_ONCE_H
#define LKPI_LINUX_CALL_ONCE_H

#include <linux/types.h>
#include <linux/mutex.h>

#define ONCE_NOT_STARTED 0
#define ONCE_RUNNING     1
#define ONCE_COMPLETED   2

struct once {
	atomic_t state;
	struct mutex lock;
};

static inline void __once_init(struct once *once, const char *name,
			       struct lock_class_key *key)
{
	(void)name;
	(void)key;
	atomic_set(&once->state, ONCE_NOT_STARTED);
	mutex_init(&once->lock);
}

#define once_init(once) __once_init((once), #once, NULL)

/* Run cb(once) until it succeeds once; every caller returns after it ran.
 * A failing cb (non-zero) leaves the once to be tried again. */
static inline int call_once(struct once *once, int (*cb)(struct once *))
{
	int r = 0;

	if (atomic_read_acquire(&once->state) == ONCE_COMPLETED)
		return 0;
	mutex_lock(&once->lock);
	if (atomic_read(&once->state) == ONCE_NOT_STARTED) {
		atomic_set(&once->state, ONCE_RUNNING);
		r = cb(once);
		atomic_set_release(&once->state, r ? ONCE_NOT_STARTED : ONCE_COMPLETED);
	}
	mutex_unlock(&once->lock);
	return r;
}

#endif
