/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_LOG2_H
#define LKPI_LINUX_LOG2_H
#include <linux/types.h>
static inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline int ffs(int x) { return __builtin_ffs(x); }
/* A macro rather than an inline: imported code puts it inside BUILD_BUG_ON,
 * which needs an integer constant expression. */
#define is_power_of_2(n) ((n) != 0 && (((n) & ((n) - 1)) == 0))
static inline unsigned long __roundup_pow_of_two(unsigned long n)
{ return n <= 1 ? 1 : 1UL << fls64(n - 1); }
#define roundup_pow_of_two(n) __roundup_pow_of_two(n)
static inline int __ilog2_u64(u64 n) { return n ? fls64(n) - 1 : 0; }
/* Constant-folded when the argument is constant. It has to be: imported code
 * uses ilog2() in array designators and in BUILD_BUG_ON, where a function
 * call is not an integer constant expression. */
#define ilog2(n) (__builtin_constant_p(n)                                  \
	? ((n) < 2 ? 0 : 63 - __builtin_clzll((unsigned long long)(n)))        \
	: __ilog2_u64(n))
#define order_base_2(n) ilog2(__roundup_pow_of_two(n))
#endif
