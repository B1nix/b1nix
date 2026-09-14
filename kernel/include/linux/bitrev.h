/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BITREV_H
#define LKPI_LINUX_BITREV_H

#include <linux/types.h>

/*
 * Bit reversal: the value with its bits in the opposite order.
 *
 * zlib's Huffman tree builder needs it — a code is emitted most-significant bit
 * first while the tree is built least-significant first, so the codes are
 * reversed as they are assigned. A wrong implementation produces a stream that
 * decompresses to garbage rather than failing, which is why it is written as
 * the obvious loop rather than a table.
 */
static inline u8 bitrev8(u8 x)
{
	x = (u8)(((x & 0xaa) >> 1) | ((x & 0x55) << 1));
	x = (u8)(((x & 0xcc) >> 2) | ((x & 0x33) << 2));
	return (u8)((x >> 4) | (x << 4));
}

static inline u16 bitrev16(u16 x)
{
	return (u16)((bitrev8((u8)x) << 8) | bitrev8((u8)(x >> 8)));
}

static inline u32 bitrev32(u32 x)
{
	return ((u32)bitrev16((u16)x) << 16) | bitrev16((u16)(x >> 16));
}

#define __bitrev8  bitrev8
#define __bitrev16 bitrev16
#define __bitrev32 bitrev32

#endif
