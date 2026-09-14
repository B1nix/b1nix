/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PERCPU_RWSEM_H
#define LKPI_LINUX_PERCPU_RWSEM_H

#include <linux/rwsem.h>

/*
 * A reader/writer semaphore whose read side is nearly free and whose write side
 * is very expensive.
 *
 * ext4 uses one for its `s_writepages_rwsem`, and the filesystem freeze
 * machinery uses them for every level of the freeze. The asymmetry is the whole
 * point upstream: readers touch a per-CPU counter, writers wait for an RCU
 * grace period.
 *
 * Here it is the ordinary rwsem. The read side therefore costs an atomic rather
 * than a per-CPU increment, which is slower and correct — and correctness is
 * what these guard: a freeze that lets one writer through has let a write past
 * a barrier the filesystem believes nothing crossed.
 */

struct percpu_rw_semaphore {
	struct rw_semaphore sem;
};

static inline int percpu_init_rwsem(struct percpu_rw_semaphore *s)
{
	init_rwsem(&s->sem);
	return 0;
}

static inline void percpu_free_rwsem(struct percpu_rw_semaphore *s) { (void)s; }

static inline void percpu_down_read(struct percpu_rw_semaphore *s)
{ down_read(&s->sem); }
static inline int percpu_down_read_trylock(struct percpu_rw_semaphore *s)
{ return down_read_trylock(&s->sem); }
static inline void percpu_up_read(struct percpu_rw_semaphore *s)
{ up_read(&s->sem); }
static inline void percpu_down_write(struct percpu_rw_semaphore *s)
{ down_write(&s->sem); }
static inline void percpu_up_write(struct percpu_rw_semaphore *s)
{ up_write(&s->sem); }
static inline bool percpu_is_read_locked(struct percpu_rw_semaphore *s)
{ return rwsem_is_locked(&s->sem); }
static inline bool percpu_is_write_locked(struct percpu_rw_semaphore *s)
{ return rwsem_is_locked(&s->sem); }

#define DEFINE_STATIC_PERCPU_RWSEM(name) \
	static struct percpu_rw_semaphore name = { __RWSEM_INITIALIZER(name.sem) }

#define percpu_rwsem_assert_held(sem) do { (void)(sem); } while (0)
#define percpu_rwsem_is_held(sem)     percpu_is_read_locked(sem)

#endif
