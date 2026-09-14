/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BIT_SPINLOCK_H
#define LKPI_LINUX_BIT_SPINLOCK_H

#include <linux/compiler.h>
#include <lkpi/env.h>

/*
 * The bit operations are open-coded on __atomic rather than taken from
 * <linux/bitops.h>, for the same reason <linux/wait_bit.h> does it: this header
 * is reached from <linux/list_bl.h> on the <linux/fs.h> chain, which starts
 * inside <linux/types.h> — so bitops.h's guard is set and its bodies are not
 * defined yet. Same operations, no cycle.
 */
#define LKPI_BSL_MASK(nr) (1ul << ((nr) % (8 * sizeof(long))))
#define LKPI_BSL_WORD(addr, nr) (&(addr)[(nr) / (8 * sizeof(long))])

/*
 * A spinlock that is one bit of a word the caller already has.
 *
 * Used where a lock per object would double the object: the buffer-head state
 * word and the hash-list head in <linux/list_bl.h>. Preemption is disabled
 * while it is held, exactly as upstream does, because the holder must not lose
 * the CPU with a bit set that nobody else can identify the owner of.
 */

static inline void bit_spin_lock(int bitnum, unsigned long *addr)
{
	unsigned long mask = LKPI_BSL_MASK(bitnum);
	unsigned long *word = LKPI_BSL_WORD(addr, bitnum);

	lkpi_preempt_disable();
	while (__atomic_fetch_or(word, mask, __ATOMIC_ACQUIRE) & mask) {
		lkpi_preempt_enable();
		do {
			lkpi_cpu_relax();
		} while (__atomic_load_n(word, __ATOMIC_RELAXED) & mask);
		lkpi_preempt_disable();
	}
}

static inline int bit_spin_trylock(int bitnum, unsigned long *addr)
{
	unsigned long mask = LKPI_BSL_MASK(bitnum);
	unsigned long *word = LKPI_BSL_WORD(addr, bitnum);

	lkpi_preempt_disable();
	if (__atomic_fetch_or(word, mask, __ATOMIC_ACQUIRE) & mask) {
		lkpi_preempt_enable();
		return 0;
	}
	return 1;
}

static inline void bit_spin_unlock(int bitnum, unsigned long *addr)
{
	__atomic_fetch_and(LKPI_BSL_WORD(addr, bitnum), ~LKPI_BSL_MASK(bitnum),
	                   __ATOMIC_RELEASE);
	lkpi_preempt_enable();
}

/* The non-atomic release. Upstream distinguishes it because the store is
 * already ordered by the release semantics of the clear; the distinction is
 * kept so the two names do not silently become one. */
static inline void __bit_spin_unlock(int bitnum, unsigned long *addr)
{
	__atomic_fetch_and(LKPI_BSL_WORD(addr, bitnum), ~LKPI_BSL_MASK(bitnum),
	                   __ATOMIC_RELEASE);
	lkpi_preempt_enable();
}

static inline int bit_spin_is_locked(int bitnum, unsigned long *addr)
{
	return (__atomic_load_n(LKPI_BSL_WORD(addr, bitnum), __ATOMIC_ACQUIRE) &
	        LKPI_BSL_MASK(bitnum)) != 0;
}

#endif
