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

#define hweight32(x) __builtin_popcount(x)
#define hweight64(x) __builtin_popcountll(x)
#endif
