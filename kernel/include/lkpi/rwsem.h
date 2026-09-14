/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_RWSEM_H
#define LKPI_RWSEM_H

#include <lkpi/types.h>

/*
 * Sleeping reader/writer semaphore.
 *
 * The filesystems are built on this the way the DRM core is built on the mutex:
 * btrfs takes `fs_info->subvol_sem` and every tree root's lock as a rwsem, and
 * ext4 guards its group descriptors with one. A rwsem is not a mutex with two
 * names — a path that takes the read side twice on one task (btrfs's backref
 * walk does) deadlocks against an exclusive lock and does not against a shared
 * one, so collapsing them would wedge the machine rather than merely slow it.
 *
 * Writers are preferred: once one is waiting, new readers queue behind it. The
 * alternative starves writers under a steady read load, which for a filesystem
 * means a transaction commit that never runs.
 *
 * It parks the caller, so it may be held across a sleep and must never be taken
 * from an interrupt handler.
 */
struct lkpi_rwsem {
	/* Readers holding it. A writer holds it when `writer` is set, and the two
	 * are never both non-zero. */
	volatile int readers;
	volatile u32 writer;
	/* Writers parked or about to park. New readers defer to this. */
	volatile u32 writers_waiting;
	/* Task id + 1 of the write holder, 0 when none — so a recursive write
	 * acquire is reportable rather than a silent self-deadlock. */
	volatile usize owner;
	volatile int guard; /* a b1nix spinlock; see the note in <lkpi/lock.h> */
};

void lkpi_rwsem_init(struct lkpi_rwsem *s);
void lkpi_rwsem_down_read(struct lkpi_rwsem *s);
int lkpi_rwsem_down_read_trylock(struct lkpi_rwsem *s);
void lkpi_rwsem_up_read(struct lkpi_rwsem *s);
void lkpi_rwsem_down_write(struct lkpi_rwsem *s);
int lkpi_rwsem_down_write_trylock(struct lkpi_rwsem *s);
void lkpi_rwsem_up_write(struct lkpi_rwsem *s);
/* Drop the write side and keep the read side, without ever releasing the
 * semaphore in between — imported code uses it to publish a structure it has
 * just built and then keep reading it. */
void lkpi_rwsem_downgrade_write(struct lkpi_rwsem *s);
/* 1 when held either way. Diagnostic only: true the instant it is read, and
 * nothing more. */
int lkpi_rwsem_is_locked(const struct lkpi_rwsem *s);
/* 1 when the calling task holds the write side. */
int lkpi_rwsem_is_write_owner(const struct lkpi_rwsem *s);

/*
 * Counting semaphore.
 *
 * Here rather than in a file of its own because it is the same wait discipline
 * with a different predicate, and because there is exactly one user: btrfs
 * serialises its UUID-tree rescan with a semaphore initialised to one. Nothing
 * would be clarified by a second header saying so.
 */
struct lkpi_semaphore {
	volatile int count;
	volatile int guard; /* a b1nix spinlock */
};

void lkpi_sema_init(struct lkpi_semaphore *s, int count);
void lkpi_sema_down(struct lkpi_semaphore *s);
/* 0 when the count was taken without sleeping, 1 when it was not — upstream's
 * inverted convention for down_trylock, kept so callers read the same. */
int lkpi_sema_trydown(struct lkpi_semaphore *s);
void lkpi_sema_up(struct lkpi_semaphore *s);

#endif
