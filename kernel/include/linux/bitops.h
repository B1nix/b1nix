/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_BITOPS_H
#define LKPI_LINUX_BITOPS_H
#include <linux/bits.h>
#include <linux/log2.h>
/* Atomic bit operations on a long array. The atomicity is not decorative: these
 * are how imported code flips per-object status flags from more than one CPU. */
static inline void set_bit(unsigned int nr, volatile unsigned long *addr)
{ __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_ACQ_REL); }
static inline void clear_bit(unsigned int nr, volatile unsigned long *addr)
{ __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_ACQ_REL); }
static inline int test_bit(unsigned int nr, const volatile unsigned long *addr)
{ return (__atomic_load_n(&addr[BIT_WORD(nr)], __ATOMIC_ACQUIRE) & BIT_MASK(nr)) != 0; }
static inline int test_and_set_bit(unsigned int nr, volatile unsigned long *addr)
{ unsigned long old = __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_ACQ_REL); return (old & BIT_MASK(nr)) != 0; }
static inline int test_and_clear_bit(unsigned int nr, volatile unsigned long *addr)
{ unsigned long old = __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_ACQ_REL); return (old & BIT_MASK(nr)) != 0; }
/* Walk the set bits of a mask. `bit` is the loop variable. */
#define for_each_set_bit(bit, addr, size)                        \
	for ((bit) = 0; (bit) < (size); (bit)++)                     \
		if (test_bit((bit), (addr)))

/* Set or clear a run of bits. Not atomic, unlike the single-bit operations
 * above: callers hold a lock over the whole range. */
/* The non-atomic single-bit operations. Callers use these when they already
 * hold a lock over the word, and the difference from the atomic ones above is
 * deliberate rather than an oversight. */
static inline void __set_bit(unsigned int nr, volatile unsigned long *addr)
{ addr[BIT_WORD(nr)] |= BIT_MASK(nr); }
static inline void __clear_bit(unsigned int nr, volatile unsigned long *addr)
{ addr[BIT_WORD(nr)] &= ~BIT_MASK(nr); }
static inline int __test_and_set_bit(unsigned int nr, volatile unsigned long *addr)
{ int was = test_bit(nr, addr); __set_bit(nr, addr); return was; }

/* Clear a bit and publish everything written before it — the release half of a
 * bit-lock. The ordering is the whole point: a plain clear lets the compiler or
 * the CPU move earlier stores past it, and the next holder then sees a
 * half-finished object. */
static inline void clear_bit_unlock(unsigned int nr, volatile unsigned long *addr)
{ __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_RELEASE); }
static inline int test_and_set_bit_lock(unsigned int nr, volatile unsigned long *addr)
{
	unsigned long old = __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr),
	                                      __ATOMIC_ACQUIRE);
	return (old & BIT_MASK(nr)) != 0;
}

static inline void bitmap_set(unsigned long *map, unsigned int start,
                              unsigned int nbits)
{
	for (unsigned int i = 0; i < nbits; i++)
		map[BIT_WORD(start + i)] |= BIT_MASK(start + i);
}

static inline void bitmap_clear(unsigned long *map, unsigned int start,
                                unsigned int nbits)
{
	for (unsigned int i = 0; i < nbits; i++)
		map[BIT_WORD(start + i)] &= ~BIT_MASK(start + i);
}

static inline void bitmap_zero(unsigned long *map, unsigned int nbits)
{
	for (unsigned int i = 0; i < BITS_TO_LONGS(nbits); i++)
		map[i] = 0;
}

#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

/*
 * The multi-word predicates.
 *
 * They live here beside bitmap_zero and bitmap_clear rather than in
 * <linux/bitmap.h>, because that is where drivers reach them from — several
 * i915 files call bitmap_empty having included only this header, and splitting
 * the set across two headers means half of it is missing at every such call.
 *
 * The trailing partial word is the whole difficulty: everything above `nbits`
 * must read as zero, because a count over the full word would include bits
 * nobody set.
 */
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

static inline unsigned int bitmap_weight(const unsigned long *src,
                                         unsigned int nbits)
{
	unsigned int len = BITS_TO_LONGS(nbits), w = 0;

	if (!len)
		return 0;
	for (unsigned int i = 0; i + 1 < len; i++)
		w += (unsigned int)__builtin_popcountl(src[i]);
	w += (unsigned int)__builtin_popcountl(
		src[len - 1] &
		(nbits % BITS_PER_LONG ? BITMAP_LAST_WORD_MASK(nbits) : ~0UL));
	return w;
}

static inline _Bool bitmap_empty(const unsigned long *src, unsigned int nbits)
{ return bitmap_weight(src, nbits) == 0; }

static inline _Bool bitmap_full(const unsigned long *src, unsigned int nbits)
{ return bitmap_weight(src, nbits) == nbits; }

static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
	unsigned int len = BITS_TO_LONGS(nbits);

	for (unsigned int i = 0; i < len; i++)
		dst[i] = ~0UL;
	if (len && nbits % BITS_PER_LONG)
		dst[len - 1] &= BITMAP_LAST_WORD_MASK(nbits);
}

static inline void bitmap_copy(unsigned long *dst, const unsigned long *src,
                               unsigned int nbits)
{ for (unsigned int i = 0; i < BITS_TO_LONGS(nbits); i++) dst[i] = src[i]; }

static inline void bitmap_or(unsigned long *dst, const unsigned long *a,
                             const unsigned long *b, unsigned int nbits)
{ for (unsigned int i = 0; i < BITS_TO_LONGS(nbits); i++) dst[i] = a[i] | b[i]; }

static inline void bitmap_and(unsigned long *dst, const unsigned long *a,
                              const unsigned long *b, unsigned int nbits)
{ for (unsigned int i = 0; i < BITS_TO_LONGS(nbits); i++) dst[i] = a[i] & b[i]; }

static inline void bitmap_andnot(unsigned long *dst, const unsigned long *a,
                                 const unsigned long *b, unsigned int nbits)
{ for (unsigned int i = 0; i < BITS_TO_LONGS(nbits); i++) dst[i] = a[i] & ~b[i]; }

static inline void bitmap_xor(unsigned long *dst, const unsigned long *a,
                              const unsigned long *b, unsigned int nbits)
{ for (unsigned int i = 0; i < BITS_TO_LONGS(nbits); i++) dst[i] = a[i] ^ b[i]; }

static inline _Bool bitmap_intersects(const unsigned long *a,
                                      const unsigned long *b, unsigned int nbits)
{
	unsigned int len = BITS_TO_LONGS(nbits);

	for (unsigned int i = 0; i < len; i++) {
		unsigned long v = a[i] & b[i];

		if (i + 1 == len && nbits % BITS_PER_LONG)
			v &= BITMAP_LAST_WORD_MASK(nbits);
		if (v)
			return 1;
	}
	return 0;
}

#define hweight32(x) __builtin_popcount(x)
#define hweight64(x) __builtin_popcountll(x)

/* Sign-extend from bit `index` downwards. Written as a shift pair rather than a
 * mask so the sign bit propagates; the cast to signed is what makes the right
 * shift arithmetic. */
static inline __s64 sign_extend64(__u64 value, int index)
{
	__u8 shift = 63 - index;
	return (__s64)(value << shift) >> shift;
}
static inline __s32 sign_extend32(__u32 value, int index)
{
	__u8 shift = 31 - index;
	return (__s32)(value << shift) >> shift;
}


/*
 * Finding set and clear bits by scanning.
 *
 * Written as a straight scan rather than word-at-a-time with a count-trailing-
 * zeros: the callers here iterate over masks of a few dozen bits (engines,
 * pipes, power wells), where the loop overhead is invisible and the simpler
 * form is the one that is obviously correct at the boundary. Returning `size`
 * for "not found" is the convention every caller tests against.
 */
static inline unsigned long find_next_bit(const unsigned long *addr,
                                          unsigned long size,
                                          unsigned long offset)
{
	while (offset < size && !test_bit((unsigned int)offset, addr))
		offset++;
	return offset;
}

static inline unsigned long find_next_zero_bit(const unsigned long *addr,
                                               unsigned long size,
                                               unsigned long offset)
{
	while (offset < size && test_bit((unsigned int)offset, addr))
		offset++;
	return offset;
}

static inline unsigned long find_first_bit(const unsigned long *addr,
                                           unsigned long size)
{ return find_next_bit(addr, size, 0); }

static inline unsigned long find_first_zero_bit(const unsigned long *addr,
                                                unsigned long size)
{ return find_next_zero_bit(addr, size, 0); }


/* The narrower population counts. Upstream has one per width because the
 * argument type decides how much is counted, and passing a u8 to hweight32
 * counts whatever the promotion left in the upper bits. */
#define hweight8(x)  ((unsigned int)__builtin_popcount((unsigned char)(x)))
#define hweight16(x) ((unsigned int)__builtin_popcount((unsigned short)(x)))


/* The index of the lowest set bit. Undefined for zero, as upstream's is — the
 * callers here always pass a mask they have already tested. */
static inline unsigned long __ffs(unsigned long word)
{ return (unsigned long)__builtin_ctzl(word); }
static inline unsigned long __fls(unsigned long word)
{ return (unsigned long)(63 - __builtin_clzl(word)); }
/* ffs/fls/fls64 are already defined above; only the double-underscore forms,
 * which are zero-based and undefined for zero, were missing. */


/* Allocation. It needs the heap, which is why upstream keeps it in
 * <linux/bitmap.h> — but a driver that includes only this header and calls
 * bitmap_zalloc is the case that matters here, and <linux/slab.h> costs
 * nothing to pull in. */
#include <linux/slab.h>

static inline unsigned long *bitmap_zalloc(unsigned int nbits, gfp_t flags)
{ return (unsigned long *)kzalloc(BITS_TO_LONGS(nbits) * sizeof(unsigned long), flags); }
static inline unsigned long *bitmap_alloc(unsigned int nbits, gfp_t flags)
{ return bitmap_zalloc(nbits, flags); }
static inline void bitmap_free(const unsigned long *bitmap)
{ kfree((void *)bitmap); }


/* The non-atomic forms. b1nix's bit helpers are already plain reads and writes
 * on a word the caller serialises, so these are the same operations under the
 * name that says the caller holds the lock. */
static inline int __test_and_clear_bit(unsigned int nr, volatile unsigned long *addr)
{ return test_and_clear_bit(nr, addr); }


/* Iterate the clear bits. Upstream keeps this in <linux/find.h>, which
 * <linux/bitops.h> pulls in; here it is in the header callers reach. */
#define for_each_clear_bit(bit, addr, size)                      \
	for ((bit) = 0; (bit) < (size); (bit)++)                     \
		if (test_bit((bit), (addr))) { } else

/* The multi-word operations live in <linux/bitmap.h>, and drivers reach them
 * through this header the way upstream's chain lets them. Included last so the
 * single-word primitives above are already defined when it arrives. */
#include <linux/bitmap.h>


/* Index of the first zero bit in a word. */
static inline unsigned long ffz(unsigned long word)
{ return (unsigned long)__builtin_ctzl(~word); }

#endif
