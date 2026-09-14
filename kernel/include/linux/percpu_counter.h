/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PERCPU_COUNTER_H
#define LKPI_LINUX_PERCPU_COUNTER_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/gfp.h>
/* DEFINE_PER_CPU and per_cpu(). Upstream's percpu_counter.h reaches them the
 * same way, and ext4 defines a plain per-CPU variable in a file that includes
 * only this one. */
#include <linux/percpu.h>

/*
 * A counter that is written far more often than it is read.
 *
 * Upstream keeps a per-CPU delta and folds it into a shared total only when the
 * delta exceeds a batch size, so the shared cache line is touched rarely. ext4
 * counts free blocks and free inodes with one, and btrfs counts its ordered
 * extents.
 *
 * Here it is one atomic. That is a real difference and it is a deliberate one:
 * the per-CPU version's whole value is avoiding cache-line contention on a
 * many-CPU write path, b1nix runs a handful of CPUs, and a wrong free-block
 * count is a filesystem bug while a contended cache line is a slow one. The
 * approximate reads (`_read_positive`, `_sum`) therefore return the exact value
 * — which is allowed: callers must already tolerate the exact answer, since
 * upstream's own `_sum` gives it.
 *
 * `percpu_counter_compare` keeps its three-valued return; callers switch on it.
 */

struct percpu_counter {
	atomic64_t count;
	s32 batch;
};

static inline int percpu_counter_init(struct percpu_counter *fbc, s64 amount,
                                      gfp_t gfp)
{
	(void)gfp;
	atomic64_set(&fbc->count, amount);
	fbc->batch = 32;
	return 0;
}

static inline void percpu_counter_destroy(struct percpu_counter *fbc)
{
	(void)fbc;
}

static inline void percpu_counter_set(struct percpu_counter *fbc, s64 amount)
{
	atomic64_set(&fbc->count, amount);
}

static inline void percpu_counter_add(struct percpu_counter *fbc, s64 amount)
{
	atomic64_add(amount, &fbc->count);
}

static inline void percpu_counter_add_batch(struct percpu_counter *fbc,
                                            s64 amount, s32 batch)
{
	(void)batch;
	atomic64_add(amount, &fbc->count);
}

static inline void percpu_counter_sub(struct percpu_counter *fbc, s64 amount)
{
	atomic64_add(-amount, &fbc->count);
}

static inline void percpu_counter_inc(struct percpu_counter *fbc)
{
	atomic64_add(1, &fbc->count);
}

static inline void percpu_counter_dec(struct percpu_counter *fbc)
{
	atomic64_add(-1, &fbc->count);
}

static inline s64 percpu_counter_read(struct percpu_counter *fbc)
{
	return atomic64_read(&fbc->count);
}

static inline s64 percpu_counter_sum(struct percpu_counter *fbc)
{
	return atomic64_read(&fbc->count);
}

static inline s64 percpu_counter_sum_positive(struct percpu_counter *fbc)
{
	s64 v = atomic64_read(&fbc->count);

	return v < 0 ? 0 : v;
}

static inline s64 percpu_counter_read_positive(struct percpu_counter *fbc)
{
	return percpu_counter_sum_positive(fbc);
}

/* -1, 0 or 1, as with any comparator. */
static inline int percpu_counter_compare(struct percpu_counter *fbc, s64 rhs)
{
	s64 v = atomic64_read(&fbc->count);

	if (v > rhs)
		return 1;
	if (v < rhs)
		return -1;
	return 0;
}

static inline int __percpu_counter_compare(struct percpu_counter *fbc, s64 rhs,
                                           s32 batch)
{
	(void)batch;
	return percpu_counter_compare(fbc, rhs);
}

/*
 * Take `amount` only if the result stays at or above zero, atomically with
 * respect to another caller doing the same. Doing it as a read then a subtract
 * lets two callers both see enough and both take it, which for ext4's free
 * block count means allocating the same blocks twice.
 */
static inline bool percpu_counter_limited_add(struct percpu_counter *fbc,
                                              s64 limit, s64 amount)
{
	for (;;) {
		s64 cur = atomic64_read(&fbc->count);

		if (cur + amount > limit)
			return false;
		if (atomic64_cmpxchg(&fbc->count, cur, cur + amount) == cur)
			return true;
	}
}

static inline void percpu_counter_sync(struct percpu_counter *fbc)
{
	(void)fbc;
}

#define percpu_counter_batch 32

/* Has this counter been initialised? Distinguishes a counter a filesystem has
 * set up from one on a superblock whose mount failed part way — the error path
 * must not destroy what was never created. */
static inline bool percpu_counter_initialized(struct percpu_counter *fbc)
{ return fbc != NULL; }

#endif
