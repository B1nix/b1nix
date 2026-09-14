/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_INT_LOG_H
#define LKPI_LINUX_INT_LOG_H
#include <linux/types.h>

/*
 * Integer logarithms in 8.24 fixed point: the integer part from the highest
 * set bit, 24 fraction bits by repeated squaring of the normalised mantissa.
 * value must be nonzero.
 */
static inline unsigned int intlog2(u32 value)
{
	unsigned int msb = 31 - (unsigned int)__builtin_clz(value);
	u64 x = (u64)value << (31 - msb);  /* 1.31, in [1, 2) */
	unsigned int frac = 0;
	int i;

	for (i = 23; i >= 0; i--) {
		x = (x * x) >> 31;
		if (x >= (1ull << 32)) {
			x >>= 1;
			frac |= 1u << i;
		}
	}
	return (msb << 24) | frac;
}

/* log10(2) in 1.31 fixed point is 646456993. */
static inline unsigned int intlog10(u32 value)
{ return (unsigned int)(((u64)intlog2(value) * 646456993ull) >> 31); }

#endif
