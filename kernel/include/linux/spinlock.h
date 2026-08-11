/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SPINLOCK_H
#define LKPI_LINUX_SPINLOCK_H

#include <linux/types.h>
#include <lkpi/lock.h>

/*
 * Spinlocks, onto lkpi's.
 *
 * A typedef and real functions, not macros. The macro version worked until it
 * did not: `spinlock_t` as a macro is substituted wherever the word appears,
 * including in a struct declared before this header was reached, and the result
 * is a member whose declared type and whose use disagree — reported as an
 * unrelated pointer-conversion error somewhere else entirely. The same hazard
 * cost a day on `mutex`, which is also a member name in imported structs.
 *
 * Every variant here disables interrupts, including the ones Linux spells
 * without _irqsave. b1nix forbids a plain spin_lock on any lock an interrupt
 * handler can also take, and a driver lock generally is one; making the plain
 * spelling the safe one costs a flags word and removes a class of same-CPU
 * self-deadlock that is otherwise found at runtime, on hardware.
 */

typedef struct lkpi_spinlock spinlock_t;
typedef struct lkpi_spinlock raw_spinlock_t;
typedef struct lkpi_spinlock rwlock_t;

#define DEFINE_SPINLOCK(name) spinlock_t name

static inline void spin_lock_init(spinlock_t *l) { lkpi_spin_lock_init(l); }
static inline void spin_lock(spinlock_t *l) { lkpi_spin_lock(l); }
static inline void spin_unlock(spinlock_t *l) { lkpi_spin_unlock(l); }
/* A real trylock: it reports failure instead of waiting. See the note in
 * <lkpi/lock.h> for why that distinction is not optional. */
static inline int spin_trylock(spinlock_t *l) { return lkpi_spin_trylock(l); }

#define spin_lock_irq(l)             spin_lock(l)
#define spin_unlock_irq(l)           spin_unlock(l)
#define spin_lock_bh(l)              spin_lock(l)
#define spin_unlock_bh(l)            spin_unlock(l)

/* The caller's flags variable is kept because imported code declares and passes
 * one; the state that matters is saved in the lock itself. */
#define spin_lock_irqsave(l, f)      do { (f) = 0; spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); spin_unlock(l); } while (0)

#define raw_spin_lock(l)             spin_lock(l)
#define raw_spin_unlock(l)           spin_unlock(l)
#define raw_spin_lock_init(l)        spin_lock_init(l)

/* Reader/writer locks map onto the exclusive one: a driver taking a read lock
 * while an interrupt takes it for writing has the same deadlock the plain lock
 * has, so this is strictly less concurrency and never less safety. */
#define rwlock_init(l)               spin_lock_init(l)
#define read_lock(l)                 spin_lock(l)
#define read_unlock(l)               spin_unlock(l)
#define write_lock(l)                spin_lock(l)
#define write_unlock(l)              spin_unlock(l)

#define assert_spin_locked(l)        do { (void)(l); } while (0)


/* Decrement, and take the lock only if the count reached zero. The point is
 * that the lock is not taken on the common path — the caller only needs it to
 * tear the object down, and taking it every time would serialise every put. */
#define atomic_dec_and_lock_irqsave(atom, lock, flags)        \
	({                                                        \
		int __z = atomic_dec_and_test(atom);                  \
		(void)(flags);                                        \
		if (__z)                                              \
			spin_lock(lock);                                  \
		__z;                                                  \
	})
#define atomic_dec_and_lock(atom, lock)                       \
	({                                                        \
		int __z = atomic_dec_and_test(atom);                  \
		if (__z)                                              \
			spin_lock(lock);                                  \
		__z;                                                  \
	})

/* Interrupt control, spelled the way imported code asks for it. b1nix's own
 * primitives are the crossing point — see <lkpi/env.h>. */
static inline void local_irq_disable(void) { (void)lkpi_irq_save(); }
static inline void local_irq_enable(void) { lkpi_irq_restore(1); }
#define local_irq_save(flags)    do { (flags) = lkpi_irq_save(); } while (0)
#define local_irq_restore(flags) lkpi_irq_restore(flags)


/* The trylock flavours. spin_trylock() already disables interrupts and saves
 * the flags inside the lock — see <lkpi/lock.h> — so the _irq and _irqsave
 * spellings are the same operation, and the caller's flags variable is unused
 * for the same reason spin_lock_irqsave()'s is. */
#define spin_trylock_irq(l)          spin_trylock(l)
#define spin_trylock_irqsave(l, f)   ({ (f) = 0; spin_trylock(l); })
#define spin_trylock_bh(l)           spin_trylock(l)

/* Lockdep nesting subclasses. There is no lockdep here, so the subclass is
 * inert and the lock is taken exactly as the unsuffixed form would. */
#define spin_lock_nested(l, sub)             do { (void)(sub); spin_lock(l); } while (0)
#define spin_lock_irqsave_nested(l, f, sub)  do { (void)(sub); spin_lock_irqsave(l, f); } while (0)

#include <linux/refcount.h>
/* Drop a reference and, if it was the last, return holding the lock — so the
 * caller can tear down under it without a window where the count is zero and
 * the lock is not held. b1nix's spinlock has no non-blocking acquire, so the
 * lock is taken first and released again when the count did not reach zero. */
static inline bool refcount_dec_and_lock_irqsave(refcount_t *r, spinlock_t *lock,
                                                 unsigned long *flags)
{
	spin_lock_irqsave(lock, *flags);
	if (refcount_dec_and_test(r))
		return true;
	spin_unlock_irqrestore(lock, *flags);
	return false;
}

#endif
