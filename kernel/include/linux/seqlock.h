/* SPDX-License-Identifier: GPL-2.0-only */
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


/*
 * A sequence counter whose writers are serialised by a named mutex.
 *
 * Upstream's variant exists so lockdep can check that every writer really does
 * hold the mutex it claims. There is no lockdep here, so the association is not
 * verified and the counter behaves exactly like the plain one — which is why it
 * is a typedef of it rather than a copy: two structs that must stay in step is
 * a way for them not to.
 */
typedef seqcount_t seqcount_mutex_t;
#define seqcount_mutex_init(s, m) do { (void)(m); seqcount_init(s); } while (0)
#define seqcount_mutex_t_init(s, m) seqcount_mutex_init(s, m)


/* The sequence a seqcount currently holds, whatever flavour of seqcount it is.
 * Upstream needs the indirection because its variants wrap the counter in
 * different structures; here they are all the same type, so this is a field
 * read — but the name has to exist because the macros call it. */
#define seqprop_sequence(s) __atomic_load_n(&(s)->sequence, __ATOMIC_ACQUIRE)


/* Force every concurrent reader to retry, without a matching write section:
 * upstream bumps the sequence by two so the count stays even. Same here. */
#define write_seqcount_invalidate(s) do { (s)->sequence += 2; } while (0)

#endif
