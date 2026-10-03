/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SWAB_H
#define LKPI_LINUX_SWAB_H

/*
 * Byte swapping, in the spelling imported code uses when it wants the swap
 * itself rather than a conversion to or from a particular byte order.
 * <linux/byteorder.h> is where they are defined; both names reach the same
 * builtins.
 */
#include <linux/byteorder.h>

#ifndef __swab16
#define __swab16(x) swab16(x)
#define __swab32(x) swab32(x)
#define __swab64(x) swab64(x)
#endif

/* Byte swaps the compiler folds when the argument is a constant. */
#define ___constant_swab16(x) ((u16)__builtin_bswap16((u16)(x)))
#define ___constant_swab32(x) ((u32)__builtin_bswap32((u32)(x)))
#define ___constant_swab64(x) ((u64)__builtin_bswap64((u64)(x)))

#endif
