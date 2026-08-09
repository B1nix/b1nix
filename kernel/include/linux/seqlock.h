/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SEQLOCK_H
#define LKPI_LINUX_SEQLOCK_H
#include <linux/spinlock.h>
#include <linux/types.h>
/*
 * Sequence locks: readers retry instead of blocking writers.
 *
 * A writer bumps the counter odd on entry and even on exit; a reader samples
 * it, reads, and samples again, retrying if it changed or was odd. The reader
 * takes no lock at all, which is why a driver uses this for state a frame
 * timestamp is read out of on every vblank.
 */
typedef struct { unsigned sequence; spinlock_t lock; } seqlock_t;
typedef struct { unsigned sequence; } seqcount_t;

static inline void seqlock_init(seqlock_t *sl)
{ sl->sequence = 0; spin_lock_init(&sl->lock); }
static inline void seqcount_init(seqcount_t *s) { s->sequence = 0; }

static inline unsigned read_seqbegin(const seqlock_t *sl)
{ return __atomic_load_n(&sl->sequence, __ATOMIC_ACQUIRE); }
static inline int read_seqretry(const seqlock_t *sl, unsigned start)
{ return (start & 1) || __atomic_load_n(&sl->sequence, __ATOMIC_ACQUIRE) != start; }

static inline void write_seqlock(seqlock_t *sl)
{ spin_lock(&sl->lock); __atomic_fetch_add(&sl->sequence, 1, __ATOMIC_ACQ_REL); }
static inline void write_sequnlock(seqlock_t *sl)
{ __atomic_fetch_add(&sl->sequence, 1, __ATOMIC_ACQ_REL); spin_unlock(&sl->lock); }

#define read_seqcount_begin(s)      __atomic_load_n(&(s)->sequence, __ATOMIC_ACQUIRE)
#define read_seqcount_retry(s, st)  (((st) & 1) || (s)->sequence != (st))
#define write_seqcount_begin(s)     __atomic_fetch_add(&(s)->sequence, 1, __ATOMIC_ACQ_REL)
#define write_seqcount_end(s)       __atomic_fetch_add(&(s)->sequence, 1, __ATOMIC_ACQ_REL)
#endif
