#ifndef B1NIX_RWLOCK_H
#define B1NIX_RWLOCK_H

#include <b1nix/types.h>

/* Read-write spinlock for kernel-internal data that is overwhelmingly read
 * (the VFS parent/sibling chain is the canonical client). Many concurrent
 * readers, exclusive writer; reader-preference (writers may starve under a
 * steady stream of readers — fine for our access pattern where mutations
 * are rare).
 *
 * State encoding (signed atomic int):
 *   0      — unlocked
 *   N > 0  — N concurrent readers hold the lock
 *  -1      — a writer holds the lock exclusively
 *
 * The lock currently runs under the Big Kernel Lock (M28 item 2 not yet
 * scheduled) so contention is effectively single-CPU and the lock is
 * decorative — but it is the discipline diff that turns dismantling the BKL
 * from a global refactor into a per-subsystem flip.
 *
 * IRQ-save variants are the default at the chain-walk sites: a reader that
 * yields mid-walk would let a same-CPU writer (rmdir/unlink path entered
 * from a timer-preempted task once preemption from ISR is enabled) free a
 * sibling out from under it. Disabling interrupts for the duration matches
 * the cli/sti window that the chain walkers used before this lock landed.
 */

typedef struct {
    volatile int state;
} rwlock_t;

#define RWLOCK_INIT { 0 }

static inline void rw_init(rwlock_t *lock) {
    lock->state = 0;
}

static inline void rw_read_lock(rwlock_t *lock) {
    for (;;) {
        int s = __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE);
        if (s >= 0) {
            int expected = s;
            if (__atomic_compare_exchange_n(&lock->state, &expected, s + 1,
                                            /*weak=*/0,
                                            __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                return;
        }
        __asm__ volatile("pause");
    }
}

static inline void rw_read_unlock(rwlock_t *lock) {
    __atomic_sub_fetch(&lock->state, 1, __ATOMIC_RELEASE);
}

static inline void rw_write_lock(rwlock_t *lock) {
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&lock->state, &expected, -1,
                                        /*weak=*/0,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return;
        __asm__ volatile("pause");
    }
}

static inline void rw_write_unlock(rwlock_t *lock) {
    __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE);
}

static inline void rw_read_lock_irqsave(rwlock_t *lock, u64 *flags) {
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags) : : "memory");
    rw_read_lock(lock);
}

static inline void rw_read_unlock_irqrestore(rwlock_t *lock, u64 flags) {
    rw_read_unlock(lock);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory");
}

static inline void rw_write_lock_irqsave(rwlock_t *lock, u64 *flags) {
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags) : : "memory");
    rw_write_lock(lock);
}

static inline void rw_write_unlock_irqrestore(rwlock_t *lock, u64 flags) {
    rw_write_unlock(lock);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory");
}

#endif /* B1NIX_RWLOCK_H */
