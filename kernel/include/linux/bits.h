/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_BITS_H
#define LKPI_LINUX_BITS_H
#include <linux/types.h>
#define BITS_PER_LONG 64
#define BIT(n)        (1UL << (n))
#define BIT_ULL(n)    (1ULL << (n))
#define BIT_MASK(nr)  (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)  ((nr) / BITS_PER_LONG)
#define BITS_PER_BYTE 8
#define BITS_PER_TYPE(t) (sizeof(t) * BITS_PER_BYTE)
#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
/* Inclusive bit range [l, h]. Written to work in preprocessor conditionals as
 * well as in code, which is why it is arithmetic rather than a function. */
#define GENMASK(h, l) \
	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) \
	(((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (64 - 1 - (h))))
#endif
