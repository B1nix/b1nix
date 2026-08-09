/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_OVERFLOW_H
#define LKPI_LINUX_OVERFLOW_H
#include <linux/types.h>
/* Checked arithmetic. The builtins report the overflow rather than wrapping,
 * which is the whole reason imported code uses these for sizes derived from
 * userspace. */
#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
#define check_sub_overflow(a, b, d) __builtin_sub_overflow(a, b, d)
static inline usize array_size(usize a, usize b)
{ usize r; return __builtin_mul_overflow(a, b, &r) ? (usize)-1 : r; }
static inline usize struct_size_helper(usize base, usize n, usize elem)
{ usize r; if (__builtin_mul_overflow(n, elem, &r)) return (usize)-1;
  return __builtin_add_overflow(base, r, &r) ? (usize)-1 : r; }
#define struct_size(p, member, n) \
	struct_size_helper(sizeof(*(p)), (n), sizeof(*(p)->member))
#define flex_array_size(p, member, n) array_size((n), sizeof(*(p)->member))
#endif
