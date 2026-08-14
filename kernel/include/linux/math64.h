/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MATH64_H
#define LKPI_LINUX_MATH64_H
#include <linux/types.h>

/* 64-bit division helpers. On x86_64 the compiler emits a single instruction
 * for each of these; they exist as functions because 32-bit Linux could not,
 * and imported source calls them by name. */
static inline u64 div_u64(u64 dividend, u32 divisor)
{
	return divisor ? dividend / divisor : 0;
}

static inline s64 div_s64(s64 dividend, s32 divisor)
{
	return divisor ? dividend / divisor : 0;
}

static inline u64 div64_u64(u64 dividend, u64 divisor)
{
	return divisor ? dividend / divisor : 0;
}

static inline s64 div64_s64(s64 dividend, s64 divisor)
{
	return divisor ? dividend / divisor : 0;
}

static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
	if (!divisor) {
		if (remainder)
			*remainder = 0;
		return 0;
	}
	if (remainder)
		*remainder = (u32)(dividend % divisor);
	return dividend / divisor;
}

static inline u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
	if (!divisor) {
		if (remainder)
			*remainder = 0;
		return 0;
	}
	if (remainder)
		*remainder = dividend % divisor;
	return dividend / divisor;
}

/* (a * mul) / div without overflowing the intermediate, which is the whole
 * reason callers reach for it. */
static inline u64 mul_u32_u32(u32 a, u32 b)
{
	return (u64)a * b;
}

/*
 * Divide in place and yield the remainder — the odd calling convention is
 * Linux's, kept because imported code writes `rem = do_div(n, d)` and expects
 * `n` to have been updated.
 */
#define do_div(n, base)                          \
	({                                           \
		u64 __rem = (u64)(n) % (u64)(base);      \
		(n) = (u64)(n) / (u64)(base);            \
		__rem;                                   \
	})

#define DIV_ROUND_CLOSEST_ULL(n, d)              \
	({                                           \
		u64 __d = (u64)(d);                      \
		__d ? ((u64)(n) + __d / 2) / __d : 0;     \
	})

static inline u64 DIV_ROUND_UP_ULL(u64 n, u32 d)
{
	return d ? (n + d - 1) / d : 0;
}


/* Rounding division on 64-bit operands. Written with the divisor subtracted
 * first so the numerator cannot overflow on the way up, which is the whole
 * reason upstream has a macro rather than the obvious expression. */
#define DIV64_U64_ROUND_UP(ll, d)     ({ u64 _d = (d); div64_u64((ll) + _d - 1, _d); })
#define DIV64_U64_ROUND_CLOSEST(ll, d) ({ u64 _d = (d); div64_u64((ll) + _d / 2, _d); })
#define DIV_ROUND_UP_ULL(ll, d)       DIV64_U64_ROUND_UP(ll, d)

#endif
