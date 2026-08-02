/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LOCK_H
#define LKPI_LOCK_H

#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <lkpi/types.h>

/*
 * Lock wrappers.
 *
 * Two kinds, and the difference is load-bearing under b1nix's rules
 * (CLAUDE.md): a `lkpi_spinlock` is a b1nix spinlock — it disables interrupts
 * and the holder must not sleep — while a `lkpi_mutex` parks the caller on a
 * scheduler wait channel and therefore MAY be held across a sleep, but must not
 * be taken from an interrupt handler.
 *
 * Driver code that takes a Linux `struct mutex` around a firmware load or a
 * fence wait maps onto lkpi_mutex; code that guards an interrupt-shared ring
 * index maps onto lkpi_spinlock.
 */

struct lkpi_spinlock {
	spinlock_t lock;
	u64 flags;
};

static inline void lkpi_spin_lock_init(struct lkpi_spinlock *l)
{
	l->lock = SPINLOCK_INIT;
	l->flags = 0;
}

/* Always the IRQ-saving variant: b1nix forbids a plain spin_lock on any lock an
 * interrupt handler can also take, and a driver lock generally is one. */
static inline void lkpi_spin_lock(struct lkpi_spinlock *l)
{
	u64 f;
	spin_lock_irqsave(&l->lock, &f);
	l->flags = f;
}

static inline void lkpi_spin_unlock(struct lkpi_spinlock *l)
{
	u64 f = l->flags;
	l->flags = 0;
	spin_unlock_irqrestore(&l->lock, f);
}

/*
 * Sleeping mutex.
 *
 * `owner` is the task id + 1 of the holder (0 = free), which makes the
 * recursive-acquire bug detectable rather than a silent self-deadlock. The
 * acquire loop uses the scheduler's two-phase wait so a release racing the
 * park cannot be lost.
 */
struct lkpi_mutex {
	volatile u32 locked;
	volatile usize owner;
	spinlock_t guard;
};

void lkpi_mutex_init(struct lkpi_mutex *m);
void lkpi_mutex_lock(struct lkpi_mutex *m);
/* Returns 1 if the lock was taken without sleeping. */
int lkpi_mutex_trylock(struct lkpi_mutex *m);
void lkpi_mutex_unlock(struct lkpi_mutex *m);
/* 1 when held by the calling task. */
int lkpi_mutex_is_locked_by_current(struct lkpi_mutex *m);

#endif
