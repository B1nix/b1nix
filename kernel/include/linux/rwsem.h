/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_RWSEM_H
#define LKPI_LINUX_RWSEM_H

#include <lkpi/rwsem.h>

/*
 * Reader/writer semaphore, onto lkpi's.
 *
 * A wrapper struct rather than a `#define rw_semaphore lkpi_rwsem`, for the
 * reason spelled out in <linux/mutex.h>: these words are also struct *member*
 * names in imported source, and a macro over one rewrites those too.
 *
 * The lock classes (`down_write_nested`, and the `_killable` forms) collapse
 * onto the plain operations. b1nix has no lockdep, so a class is nothing to
 * record; and nothing here can be killed while it waits, so the killable forms
 * cannot fail — they return 0, which callers read as "acquired". A caller that
 * checks for -EINTR still compiles and simply never sees one.
 */

struct rw_semaphore {
	struct lkpi_rwsem sem;
};

#define __RWSEM_INITIALIZER(name) { { 0, 0, 0, 0, 0 } }
#define DECLARE_RWSEM(name) \
	struct rw_semaphore name = __RWSEM_INITIALIZER(name)

static inline void init_rwsem(struct rw_semaphore *s)
{
	lkpi_rwsem_init(&s->sem);
}

static inline void down_read(struct rw_semaphore *s)
{
	lkpi_rwsem_down_read(&s->sem);
}

static inline int down_read_trylock(struct rw_semaphore *s)
{
	return lkpi_rwsem_down_read_trylock(&s->sem);
}

static inline void up_read(struct rw_semaphore *s)
{
	lkpi_rwsem_up_read(&s->sem);
}

static inline void down_write(struct rw_semaphore *s)
{
	lkpi_rwsem_down_write(&s->sem);
}

static inline int down_write_trylock(struct rw_semaphore *s)
{
	return lkpi_rwsem_down_write_trylock(&s->sem);
}

static inline void up_write(struct rw_semaphore *s)
{
	lkpi_rwsem_up_write(&s->sem);
}

static inline void downgrade_write(struct rw_semaphore *s)
{
	lkpi_rwsem_downgrade_write(&s->sem);
}

static inline int rwsem_is_locked(struct rw_semaphore *s)
{
	return lkpi_rwsem_is_locked(&s->sem);
}

/*
 * "Is somebody waiting for this?"
 *
 * Answered from the writer count alone. Reporting contention when there is none
 * costs a caller an early unlock it did not need; missing real contention costs
 * a waiting writer an unbounded wait, and it is writers that this queue can
 * starve — so the side to err on is this one.
 */
static inline int rwsem_is_contended(struct rw_semaphore *s)
{
	return s->sem.writers_waiting != 0;
}

#define down_read_nested(s, subclass)          (((void)(subclass)), down_read(s))
#define down_write_nested(s, subclass)         (((void)(subclass)), down_write(s))
#define down_read_killable(s)                  (down_read(s), 0)
#define down_write_killable(s)                 (down_write(s), 0)
#define down_write_killable_nested(s, subclass) (down_write(s), 0)

/* Assertions about who holds a rwsem. Without lockdep the owner is not
 * recorded, so only "held at all" can be checked. */
#define rwsem_assert_held_write(s)          WARN_ON(!rwsem_is_locked(s))
#define rwsem_assert_held_write_nolockdep(s) WARN_ON(!rwsem_is_locked(s))
/* A read acquisition a signal may interrupt. Nothing here delivers signals to
 * a waiter, so it always acquires. */
#define down_read_interruptible(s)          (down_read(s), 0)

#endif
