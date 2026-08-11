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

/*
 * Does `x` still fit once it is stored in type T?
 *
 * The round trip is the test: assign into the target type, read it back, and
 * see whether it changed. That catches both narrowing and a signed/unsigned
 * flip, which a range comparison written by hand usually does not — and these
 * guard buffer sizes crossing from a 64-bit computation into a 32-bit register
 * field, where being wrong means programming the hardware with a truncated
 * length.
 */
/*
 * Does `x` change when stored in the type of `T`?
 *
 * T is an *expression* of the target type, not a type name — upstream writes
 * `overflows_type(size, obj->base.size)`.
 *
 * Written as a plain expression rather than a statement-expression, because
 * these appear inside static_assert: a ({ ... }) is not a constant expression,
 * so the assertion fails to compile on values the compiler knows perfectly
 * well. The sign test is the second half — a value that round-trips bit for bit
 * can still change sign, and that is exactly the case a range check misses.
 */
#define overflows_type(x, T)                                             \
	(((__typeof__(T))(x) != (x)) ||                                      \
	 (((x) < 0) != ((__typeof__(T))(x) < 0)))

#define safe_conversion(ptr, value)                                      \
	({                                                                   \
		__typeof__(value) __v = (value);                                 \
		!overflows_type(__v, *(ptr)) ? (*(ptr) = __v, true) : false;     \
	})

/*
 * The inverse question, and the one that appears inside static_assert.
 *
 * When `x` is not a constant expression there is nothing to decide at compile
 * time, so the answer is true — upstream does the same, through
 * __builtin_choose_expr. Without that branch the assertion is handed a
 * non-constant expression and fails on code that is perfectly correct: i915
 * writes static_assert(castable_to_type(n, u32)) inside an inline whose `n` is
 * a parameter.
 */
#define castable_to_type(x, T)                                           \
	__builtin_choose_expr(__is_constexpr(x),                             \
	                      !overflows_type(x, *(T *)0),                   \
	                      1)

#endif
