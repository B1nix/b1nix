/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_ATOMIC_H
#define LKPI_LINUX_ATOMIC_H

#include <b1nix/types.h>

/* <b1nix/types.h>, not <linux/types.h>: that header includes this one (through
 * refcount.h), and a cycle leaves the atomics undefined at the point refcount
 * needs them. */

/*
 * Atomics.
 *
 * The type is a struct rather than a bare int on purpose, exactly as in Linux:
 * it makes a plain `x = counter` a compile error instead of a silently
 * non-atomic read. Every operation below is a compiler builtin that emits the
 * locked instruction directly.
 *
 * Memory ordering follows Linux's rules, which are not uniform and are the part
 * that is easy to get wrong: plain atomic_read/atomic_set are relaxed, the
 * read-modify-write operations that return a value are full barriers, and the
 * ones that do not return anything are relaxed. Making everything sequentially
 * consistent would be correct but would quietly cost on every counter; making
 * everything relaxed would break the algorithms that rely on the barrier.
 */

typedef struct {
	volatile int counter;
} atomic_t;

typedef struct {
	volatile long counter;
} atomic64_t;

#define ATOMIC_INIT(i) { (i) }

static inline int atomic_read(const atomic_t *v)
{
	return __atomic_load_n(&v->counter, __ATOMIC_RELAXED);
}

static inline void atomic_set(atomic_t *v, int i)
{
	__atomic_store_n(&v->counter, i, __ATOMIC_RELAXED);
}

static inline void atomic_inc(atomic_t *v)
{
	__atomic_fetch_add(&v->counter, 1, __ATOMIC_RELAXED);
}

static inline void atomic_dec(atomic_t *v)
{
	__atomic_fetch_sub(&v->counter, 1, __ATOMIC_RELAXED);
}

static inline void atomic_add(int i, atomic_t *v)
{
	__atomic_fetch_add(&v->counter, i, __ATOMIC_RELAXED);
}

static inline void atomic_sub(int i, atomic_t *v)
{
	__atomic_fetch_sub(&v->counter, i, __ATOMIC_RELAXED);
}

/* The value-returning forms are full barriers. */
static inline int atomic_inc_return(atomic_t *v)
{
	return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST);
}

static inline int atomic_dec_return(atomic_t *v)
{
	return __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST);
}

static inline int atomic_add_return(int i, atomic_t *v)
{
	return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_sub_return(int i, atomic_t *v)
{
	return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_fetch_add(int i, atomic_t *v)
{
	return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_fetch_sub(int i, atomic_t *v)
{
	return __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_fetch_inc(atomic_t *v)
{
	return __atomic_fetch_add(&v->counter, 1, __ATOMIC_SEQ_CST);
}

static inline int atomic_dec_and_test(atomic_t *v)
{
	return atomic_dec_return(v) == 0;
}

static inline int atomic_inc_and_test(atomic_t *v)
{
	return atomic_inc_return(v) == 0;
}

static inline int atomic_sub_and_test(int i, atomic_t *v)
{
	return atomic_sub_return(i, v) == 0;
}

static inline int atomic_cmpxchg(atomic_t *v, int old, int new_val)
{
	__atomic_compare_exchange_n(&v->counter, &old, new_val, 1, __ATOMIC_SEQ_CST,
	                            __ATOMIC_SEQ_CST);
	return old;
}

static inline int atomic_xchg(atomic_t *v, int new_val)
{
	return __atomic_exchange_n(&v->counter, new_val, __ATOMIC_SEQ_CST);
}

/* Increment unless the value is zero — the "take a reference if the object is
 * still alive" primitive. The loop is what makes it safe against a concurrent
 * drop to zero. */
static inline int atomic_add_unless(atomic_t *v, int a, int u)
{
	int c = atomic_read(v);
	while (c != u) {
		if (__atomic_compare_exchange_n(&v->counter, &c, c + a, 1,
		                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return 1;
	}
	return 0;
}

#define atomic_inc_not_zero(v) atomic_add_unless((v), 1, 0)

static inline long atomic64_read(const atomic64_t *v)
{
	return __atomic_load_n(&v->counter, __ATOMIC_RELAXED);
}

static inline void atomic64_set(atomic64_t *v, long i)
{
	__atomic_store_n(&v->counter, i, __ATOMIC_RELAXED);
}

static inline long atomic64_inc_return(atomic64_t *v)
{
	return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST);
}

static inline long atomic64_add_return(long i, atomic64_t *v)
{
	return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic64_add(long i, atomic64_t *v)
{
	__atomic_fetch_add(&v->counter, i, __ATOMIC_RELAXED);
}

static inline void atomic64_sub(long i, atomic64_t *v)
{
	__atomic_fetch_sub(&v->counter, i, __ATOMIC_RELAXED);
}

static inline void atomic64_inc(atomic64_t *v)
{
	__atomic_fetch_add(&v->counter, 1, __ATOMIC_RELAXED);
}

#define atomic_long_t atomic64_t
#define atomic_long_read(v)  atomic64_read(v)
#define atomic_long_set(v, i) atomic64_set(v, i)

#define cmpxchg(ptr, old, new_val)                                      \
	({                                                                  \
		__typeof__(*(ptr)) __o = (old);                                 \
		__atomic_compare_exchange_n((ptr), &__o, (new_val), 1,          \
		                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
		__o;                                                            \
	})

#define xchg(ptr, new_val) __atomic_exchange_n((ptr), (new_val), __ATOMIC_SEQ_CST)


/* Release-ordered store. The paired acquire is on the reader's side; what this
 * guarantees is that everything written before it is visible to anyone who then
 * observes this value. */
static inline void atomic_set_release(atomic_t *v, int i)
{ __atomic_store_n(&v->counter, i, __ATOMIC_RELEASE); }
static inline int atomic_read_acquire(const atomic_t *v)
{ return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE); }


/* A store followed by a full barrier: what a caller writes when a later reader
 * must not see the store reordered past anything after it. */
#define smp_store_mb(var, value) \
	do { __atomic_store_n(&(var), (value), __ATOMIC_SEQ_CST); } while (0)


/* Barriers around an atomic that does not already imply one. b1nix's atomics
 * are all sequentially consistent, so the ordering these ask for is already
 * there — but they are spelled out rather than left empty, because a compiler
 * barrier is still needed to stop the surrounding accesses being reordered. */
#define smp_mb__before_atomic() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_mb__after_atomic()  __atomic_thread_fence(__ATOMIC_SEQ_CST)


/* Bitwise updates. Upstream returns nothing from these and the value-returning
 * forms are spelled atomic_fetch_*; keeping that split matters because a caller
 * using the wrong one silently ignores the previous value. */
static inline void atomic_and(int i, atomic_t *v)
{ __atomic_fetch_and(&v->counter, i, __ATOMIC_ACQ_REL); }
static inline void atomic_or(int i, atomic_t *v)
{ __atomic_fetch_or(&v->counter, i, __ATOMIC_ACQ_REL); }
static inline void atomic_andnot(int i, atomic_t *v)
{ __atomic_fetch_and(&v->counter, ~i, __ATOMIC_ACQ_REL); }
static inline int atomic_fetch_and(int i, atomic_t *v)
{ return __atomic_fetch_and(&v->counter, i, __ATOMIC_ACQ_REL); }
static inline int atomic_fetch_or(int i, atomic_t *v)
{ return __atomic_fetch_or(&v->counter, i, __ATOMIC_ACQ_REL); }


/* The long-width counters are aliases of the 64-bit ones above (see line ~181):
 * b1nix is 64-bit, so `long` and the 64-bit counter are the same width and one
 * implementation serves both. Only the operations the aliases did not cover are
 * added here. */
#define atomic_long_inc(v)      atomic64_add(1, v)
#define atomic_long_dec(v)      atomic64_sub(1, v)
#define atomic_long_add(i, v)   atomic64_add(i, v)
#define atomic_long_sub(i, v)   atomic64_sub(i, v)
#define ATOMIC_LONG_INIT(i)     { (i) }


/*
 * The try_cmpxchg family: compare-and-exchange that reports success as a bool
 * and writes the observed value back through the old pointer, so a failing
 * caller does not need a second load. Same operation as cmpxchg, different
 * reporting.
 */
#define try_cmpxchg(ptr, oldp, new_val)                                  \
	__atomic_compare_exchange_n((ptr), (oldp), (new_val), false,         \
	                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define try_cmpxchg_acquire(ptr, oldp, new_val) try_cmpxchg(ptr, oldp, new_val)
#define try_cmpxchg_relaxed(ptr, oldp, new_val) try_cmpxchg(ptr, oldp, new_val)
#define atomic_try_cmpxchg(v, oldp, new_val) try_cmpxchg(&(v)->counter, (oldp), (new_val))
#define cmpxchg64(ptr, old_val, new_val) cmpxchg(ptr, old_val, new_val)


static inline long atomic64_sub_return(long i, atomic64_t *v)
{ return __atomic_sub_fetch(&v->counter, i, __ATOMIC_ACQ_REL); }

#endif
