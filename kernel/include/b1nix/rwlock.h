#ifndef B1NIX_RWLOCK_H
#define B1NIX_RWLOCK_H

#include <b1nix/types.h>
#include <b1nix/arch.h>

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

/* While spinning we MUST drain TLB shootdown IPIs ourselves: callers reach
 * here via the _irqsave variants (vmm_lock is taken IRQs-off for the whole
 * page-table walk), so the IPI delivery is masked and a shootdown initiator
 * on another CPU would never see our ACK. Same fix as the regular spinlock
 * in spinlock.h — without this, smp=4+ KVM builds panic with
 * `tlb: shootdown stalled, pending=1; [PANIC] tlb_shootdown timeout` after
 * a few seconds of -j8 build, because at least one CPU is always waiting
 * on vmm_lock for a fork/exec page-table walk. */
void tlb_shootdown_poll(void);

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
        cpu_relax();
        tlb_shootdown_poll();
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
        cpu_relax();
        tlb_shootdown_poll();
    }
}

static inline void rw_write_unlock(rwlock_t *lock) {
    __atomic_store_n(&lock->state, 0, __ATOMIC_RELEASE);
}

static inline void rw_read_lock_irqsave(rwlock_t *lock, u64 *flags) {
#ifdef __x86_64__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags) : : "memory");
#elif defined(__aarch64__)
    u64 daif;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(daif) : : "memory");
    *flags = daif;
#else
    u32 f32;
    __asm__ volatile("pushfd; popl %0; cli" : "=r"(f32) : : "memory");
    *flags = f32;
#endif
    rw_read_lock(lock);
}

static inline void rw_read_unlock_irqrestore(rwlock_t *lock, u64 flags) {
    rw_read_unlock(lock);
#ifdef __x86_64__
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
#else
    u32 f32 = (u32)flags;
    __asm__ volatile("pushl %0; popfd" : : "r"(f32) : "memory");
#endif
}

static inline void rw_write_lock_irqsave(rwlock_t *lock, u64 *flags) {
#ifdef __x86_64__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags) : : "memory");
#elif defined(__aarch64__)
    u64 daif;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(daif) : : "memory");
    *flags = daif;
#else
    u32 f32;
    __asm__ volatile("pushfd; popl %0; cli" : "=r"(f32) : : "memory");
    *flags = f32;
#endif
    rw_write_lock(lock);
}

static inline void rw_write_unlock_irqrestore(rwlock_t *lock, u64 flags) {
    rw_write_unlock(lock);
#ifdef __x86_64__
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
#else
    u32 f32 = (u32)flags;
    __asm__ volatile("pushl %0; popfd" : : "r"(f32) : "memory");
#endif
}

#endif /* B1NIX_RWLOCK_H */
