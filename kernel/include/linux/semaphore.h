/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SEMAPHORE_H
#define LKPI_LINUX_SEMAPHORE_H

#include <lkpi/rwsem.h>

/*
 * Counting semaphore, onto lkpi's.
 *
 * `down_interruptible` and `down_killable` cannot fail here — nothing in this
 * kernel interrupts a waiter on a semaphore — so they acquire and return 0.
 * `down_trylock` keeps upstream's inverted result: zero means acquired.
 */

struct semaphore {
	struct lkpi_semaphore sem;
};

#define DEFINE_SEMAPHORE(name, n) struct semaphore name = { { (n), 0 } }

static inline void sema_init(struct semaphore *s, int val)
{
	lkpi_sema_init(&s->sem, val);
}

static inline void down(struct semaphore *s) { lkpi_sema_down(&s->sem); }
static inline void up(struct semaphore *s) { lkpi_sema_up(&s->sem); }
static inline int down_trylock(struct semaphore *s)
{
	return lkpi_sema_trydown(&s->sem);
}
static inline int down_interruptible(struct semaphore *s)
{
	lkpi_sema_down(&s->sem);
	return 0;
}
static inline int down_killable(struct semaphore *s)
{
	lkpi_sema_down(&s->sem);
	return 0;
}

#endif
