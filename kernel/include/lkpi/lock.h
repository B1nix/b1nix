/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LOCK_H
#define LKPI_LOCK_H

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

/*
 * The lock word is ours rather than b1nix's `spinlock_t`, and the operations
 * are out of line, so this header pulls in no b1nix declarations. That is the
 * boundary the import needs: a translation unit compiling imported DRM source
 * includes this file, and if b1nix's spinlock.h came with it, `spinlock_t` and
 * `spin_lock` would mean two different things in one translation unit.
 *
 * The layout matches what lock.c casts it to; both are `volatile int` plus the
 * saved flags, and lock.c asserts the sizes agree so a change to either is a
 * build error rather than a silent mismatch.
 */
struct lkpi_spinlock {
	volatile int raw;
	u64 flags;
};

void lkpi_spin_lock_init(struct lkpi_spinlock *l);
/* Always the IRQ-saving variant: b1nix forbids a plain spin_lock on any lock an
 * interrupt handler can also take, and a driver lock generally is one. */
void lkpi_spin_lock(struct lkpi_spinlock *l);
void lkpi_spin_unlock(struct lkpi_spinlock *l);

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
	volatile int guard; /* a b1nix spinlock; see the note above */
};

void lkpi_mutex_init(struct lkpi_mutex *m);
void lkpi_mutex_lock(struct lkpi_mutex *m);
/* Returns 1 if the lock was taken without sleeping. */
int lkpi_mutex_trylock(struct lkpi_mutex *m);
void lkpi_mutex_unlock(struct lkpi_mutex *m);
/* 1 when held by the calling task. */
int lkpi_mutex_is_locked_by_current(struct lkpi_mutex *m);

#endif
