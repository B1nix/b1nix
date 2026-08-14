/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_HASH_H
#define LKPI_LINUX_HASH_H
#include <linux/types.h>
/* Multiplicative hashing by the golden ratio: the top bits of the product are
 * the well-mixed ones, which is why the result is shifted down rather than
 * masked. */
#define GOLDEN_RATIO_32 0x61C88647u
#define GOLDEN_RATIO_64 0x61C8864680B583EBull
static inline u32 hash_32(u32 val, unsigned int bits)
{ return (val * GOLDEN_RATIO_32) >> (32 - bits); }
static inline u32 hash_64(u64 val, unsigned int bits)
{ return (u32)((val * GOLDEN_RATIO_64) >> (64 - bits)); }
#define hash_long(val, bits) hash_64((u64)(val), bits)
#endif
