/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_WORDPART_H
#define LKPI_LINUX_WORDPART_H

/* upper/lower_32_bits are with the rest of <linux/kernel.h>. */
#include <linux/kernel.h>

#ifndef upper_16_bits
#define upper_16_bits(n) ((u16)((n) >> 16))
#define lower_16_bits(n) ((u16)((n) & 0xffff))
#endif
#ifndef REPEAT_BYTE
#define REPEAT_BYTE(x) ((~0ul / 0xff) * (x))
#endif

#endif
