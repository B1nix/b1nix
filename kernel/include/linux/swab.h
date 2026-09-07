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

#endif
