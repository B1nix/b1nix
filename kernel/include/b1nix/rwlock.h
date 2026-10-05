/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/kprof.h>
#ifndef B1NIX_RWLOCK_H
#define B1NIX_RWLOCK_H

#include <b1nix/types.h>
#include <b1nix/arch.h>
#include <b1nix/spinlock.h>

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
    /* The writer's owner token (spin_owner_self) while state is -1, so a
     * release by anyone else and a holder re-entering are caught. */
    volatile int writer;
} rwlock_t;

#define RWLOCK_INIT { 0 }

static inline void rw_init(rwlock_t *lock) {
    lock->state = 0;
    lock->writer = 0;
}

/* Shared with spinlock.h (kernel/sched/lockdep.c). */
/* A read unlock with no reader, or a write unlock by a task that is not the
 * writer (kernel/sched/lockdep.c). Does not return. */
void rw_unlock_bad(rwlock_t *lock, int write, u64 caller) __attribute__((noreturn));


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
    int self = spin_owner_self();
    u64 deadline = 0;

    for (;;) {
        int s = __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE);
        if (s >= 0) {
            int expected = s;
            if (__atomic_compare_exchange_n(&lock->state, &expected, s + 1,
                                            /*weak=*/0,
                                            __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                break;
            deadline = 0;
            continue;
        }
        /* A writer holds it. If that writer is this task, the wait never ends. */
        if (__builtin_expect(lock->writer == self && self != SPIN_OWNER_NOTASK, 0))
            spin_lock_recursive(&lock->state,
                                (u64)(usize)__builtin_return_address(0));
        cpu_relax();
        tlb_shootdown_poll();
        /* The same lockup rule as spin_lock: ten seconds without ever seeing
         * the writer let go is a deadlock, named, not a silent hang. */
        if (deadline == 0)
            deadline = spin_rdtsc() + SPIN_LOCK_STUCK_CYCLES;
        else if (spin_rdtsc() > deadline)
            spin_lock_stuck(&lock->state, (u64)(usize)__builtin_return_address(0));
    }
    spin_note(lock, SPIN_NOTE_ACQUIRE);
}

static inline void rw_read_unlock(rwlock_t *lock) {
    int left = __atomic_sub_fetch(&lock->state, 1, __ATOMIC_RELEASE);

    if (__builtin_expect(left < 0, 0))
        rw_unlock_bad(lock, 0, (u64)(usize)__builtin_return_address(0));
    spin_note(lock, SPIN_NOTE_RELEASE);
}

static inline void rw_write_lock(rwlock_t *lock) {
    int self = spin_owner_self();
    u64 deadline = 0;

    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&lock->state, &expected, -1,
                                        /*weak=*/0,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            break;
        if (__builtin_expect(expected == -1 && lock->writer == self &&
                                 self != SPIN_OWNER_NOTASK, 0))
            spin_lock_recursive(&lock->state,
                                (u64)(usize)__builtin_return_address(0));
        cpu_relax();
        tlb_shootdown_poll();
        /* Seen free since the last attempt: whoever held it is moving. */
        if (__atomic_load_n(&lock->state, __ATOMIC_RELAXED) == 0) {
            deadline = 0;
            continue;
        }
        if (deadline == 0)
            deadline = spin_rdtsc() + SPIN_LOCK_STUCK_CYCLES;
        else if (spin_rdtsc() > deadline)
            spin_lock_stuck(&lock->state, (u64)(usize)__builtin_return_address(0));
    }
    lock->writer = self;
    spin_note(lock, SPIN_NOTE_ACQUIRE);
}

/* rw_assert(RA_WLOCKED): the calling task is the writer. */
static inline void rw_assert_wlocked(rwlock_t *lock) {
    int self = spin_owner_self();

    if (__builtin_expect(lock->state != -1 ||
                             (lock->writer != self && self != SPIN_OWNER_NOTASK &&
                              lock->writer != SPIN_OWNER_NOTASK), 0))
        spin_assert_held_failed(&lock->state,
                                (u64)(usize)__builtin_return_address(0));
}

static inline void rw_write_unlock(rwlock_t *lock) {
    int self = spin_owner_self();

    if (__builtin_expect(lock->state != -1 ||
                             (lock->writer != self && self != SPIN_OWNER_NOTASK &&
                              lock->writer != SPIN_OWNER_NOTASK), 0))
        rw_unlock_bad(lock, 1, (u64)(usize)__builtin_return_address(0));
    lock->writer = 0;
    spin_note(lock, SPIN_NOTE_RELEASE);
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
    if (__builtin_expect(kprof_irqoff_on, 0) && KPROF_IRQ_WAS_ON(*flags))
        kprof_irqoff_begin(__builtin_return_address(0));
    rw_read_lock(lock);
}

static inline void rw_read_unlock_irqrestore(rwlock_t *lock, u64 flags) {
    rw_read_unlock(lock);
    if (__builtin_expect(kprof_irqoff_on, 0) && KPROF_IRQ_WAS_ON(flags))
        kprof_irqoff_end();
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
    if (__builtin_expect(kprof_irqoff_on, 0) && KPROF_IRQ_WAS_ON(*flags))
        kprof_irqoff_begin(__builtin_return_address(0));
    rw_write_lock(lock);
}

static inline void rw_write_unlock_irqrestore(rwlock_t *lock, u64 flags) {
    rw_write_unlock(lock);
    if (__builtin_expect(kprof_irqoff_on, 0) && KPROF_IRQ_WAS_ON(flags))
        kprof_irqoff_end();
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
