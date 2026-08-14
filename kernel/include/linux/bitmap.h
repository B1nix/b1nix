/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BITMAP_H
#define LKPI_LINUX_BITMAP_H
/* Upstream splits the multi-word bit operations out of <linux/bitops.h>; here
 * they are all in that header, because that is the one drivers reach them from.
 * This is the split-out name pointing back. */
#include <linux/bitops.h>

/* Is every bit of `src1` also set in `src2`? The trailing-word mask matters
 * here too: bits above nbits are not part of either set. */
static inline bool bitmap_subset(const unsigned long *src1,
                                 const unsigned long *src2, unsigned int nbits)
{
	unsigned int len = BITS_TO_LONGS(nbits);

	for (unsigned int i = 0; i < len; i++) {
		unsigned long extra = src1[i] & ~src2[i];

		if (i + 1 == len && nbits % BITS_PER_LONG)
			extra &= BITMAP_LAST_WORD_MASK(nbits);
		if (extra)
			return false;
	}
	return true;
}


/* Load a bitmap from an array of 32-bit words. On a 64-bit host two source
 * words pack into one destination word, little-endian first-word-low, which is
 * the layout the hardware tables these come from already use. */
static inline void bitmap_from_arr32(unsigned long *bitmap, const u32 *buf,
                                     unsigned int nbits)
{
	unsigned int i, nwords = (nbits + 31) / 32;
	for (i = 0; i < (nbits + BITS_PER_LONG - 1) / BITS_PER_LONG; i++)
		bitmap[i] = 0;
	for (i = 0; i < nwords; i++)
		bitmap[i / 2] |= (unsigned long)buf[i] << (32 * (i % 2));
}

static inline void bitmap_shift_right(unsigned long *dst,
                                      const unsigned long *src,
                                      unsigned int shift, unsigned int nbits)
{
	unsigned int i;
	for (i = 0; i < nbits; i++) {
		unsigned int s = i + shift;
		int v = (s < nbits) && test_bit(s, src);
		if (v)
			__set_bit(i, dst);
		else
			__clear_bit(i, dst);
	}
}

/* Reserve 2^order aligned bits, returning the offset, or -ENOMEM if no aligned
 * run of that size is free. */
static inline int bitmap_find_free_region(unsigned long *bitmap, unsigned int bits,
                                          int order)
{
	unsigned int n = 1u << order, i, j;
	for (i = 0; i + n <= bits; i += n) {
		for (j = 0; j < n; j++)
			if (test_bit(i + j, bitmap))
				break;
		if (j == n) {
			for (j = 0; j < n; j++)
				__set_bit(i + j, bitmap);
			return (int)i;
		}
	}
	return -ENOMEM;
}

static inline void bitmap_release_region(unsigned long *bitmap, unsigned int pos,
                                         int order)
{
	unsigned int n = 1u << order, j;
	for (j = 0; j < n; j++)
		__clear_bit(pos + j, bitmap);
}

#define for_each_clear_bit(bit, addr, size)                      \
	for ((bit) = 0; (bit) < (size); (bit)++)                     \
		if (test_bit((bit), (addr))) { } else

#endif
