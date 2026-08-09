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

#endif
