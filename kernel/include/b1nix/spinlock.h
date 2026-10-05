/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/kprof.h>
#ifndef B1NIX_SPINLOCK_H
#define B1NIX_SPINLOCK_H

#include <b1nix/types.h>
#include <b1nix/lockdep.h>

/* Spinlock: a compare-and-swap lock whose word names its holder.
 *
 * 0 is unlocked. A held lock carries the holder's owner token (see
 * spin_owner_self), never a bare 1: an unlock by anyone but the holder, and a
 * holder spinning on its own lock, are then caught at the moment they happen
 * instead of as a hang or as corruption later on.
 */

typedef volatile int spinlock_t;

#define SPINLOCK_INIT 0

/* Atomically exchange byte: lock cmpxchg or xchg.
 * Returns the old value. */
static inline int spin_xchg(volatile int *lock, int val) {
#if defined(__x86_64__)
    int old;
    __asm__ volatile("xchg %0, %1"
                     : "=r"(old), "+m"(*lock)
                     : "0"(val)
                     : "memory");
    return old;
#else
    return __atomic_exchange_n(lock, val, __ATOMIC_SEQ_CST);
#endif
}

/* Drain any in-flight cross-CPU TLB shootdown while spin-waiting (defined in
 * kernel/arch/x86_64/tlb.c). A CPU spinning here may have interrupts disabled (it
 * was called under cli, or via spin_lock_irqsave) and so cannot take the
 * shootdown IPI; the shootdown initiator also waits IRQs-off, so without this
 * poll the two deadlock. Fast path is a single load when nothing is pending, so
 * uniprocessor and uncontended SMP pay nothing (the loop body only runs when a
 * lock is actually contended). Always linked (tlb.c is in every kernel). */
void tlb_shootdown_poll(void);

/* Reports a spinlock that never became available (see kernel/sched/lockdep.c).
 * Does not return. */
void spin_lock_stuck(volatile int *lock, u64 caller) __attribute__((noreturn));

/* Reports an unlock of a lock nobody holds -- a second unlock, or an unlock
 * on a path that never locked (kernel/sched/lockdep.c). Does not return. */
void spin_unlock_unheld(volatile int *lock, u64 caller) __attribute__((noreturn));

/* The calling task's owner token: never 0, SPIN_OWNER_NOTASK before this CPU
 * runs a task. Derived from the task's address, so two tasks can only share a
 * token by aliasing, which can hide a violation but never invent one. */
#ifndef SPIN_OWNER_NOTASK
#define SPIN_OWNER_NOTASK 0x7fffffff
#endif
int spin_owner_self(void);

/* Every native lock acquire and release: the current task's held count (for
 * the checks that it holds none when it sleeps or returns to user mode) and
 * WITNESS, which learns the order locks are taken in and panics on a
 * reversal. A trylock is counted and recorded but cannot wait, so it is not
 * checked against the order. */
#define SPIN_NOTE_ACQUIRE 1
#define SPIN_NOTE_TRY     2
#define SPIN_NOTE_RELEASE (-1)
/* A sleeping lock (kmutex): ordered by witness, not counted as a spinlock. */
#define SPIN_NOTE_SLEEP_ACQUIRE 3
#define SPIN_NOTE_SLEEP_RELEASE (-2)
void spin_note(volatile void *lock, int op);
/* The same, naming the acquiring site: for a wrapper (linuxkpi's spinlock)
 * whose own call into spin_lock would otherwise be every lock's site. */
void spin_note_at(volatile void *lock, int op, u64 site);

/* A lock released by a task that does not hold it, and a task spinning on a
 * lock it already holds (kernel/sched/lockdep.c). Neither returns. */
void spin_unlock_foreign(volatile int *lock, int self, u64 caller)
    __attribute__((noreturn));
void spin_lock_recursive(volatile int *lock, u64 caller) __attribute__((noreturn));

/* How long a contended acquire may take before it is called a lockup.
 *
 * This used to count iterations, which is not a measure of time on a machine
 * whose CPUs are themselves virtual: a vCPU spinning here can be descheduled
 * by the host mid-loop, and on a busy host that is routine. The counter then
 * reached its limit while the lock was in fact free — the reports said
 * `value=0`, a lockup on a lock nobody held — and panicked a healthy kernel.
 * A cycle deadline measures the thing the check is actually about. Ten seconds
 * is far beyond any legitimate hold and still reports a real deadlock long
 * before a human gives up on the machine. */
#if defined(__x86_64__)
#define SPIN_LOCK_STUCK_CYCLES 30000000000ULL /* ~10 s at 3 GHz */

static inline u64 spin_rdtsc(void) {
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
#elif defined(__aarch64__)
/* The virtual counter is the architectural free-running clock; unlike the TSC
 * its rate is discoverable, so the deadline is ten real seconds on any board
 * rather than ten seconds at an assumed 3 GHz. */
static inline u64 spin_rdtsc(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline u64 spin_stuck_cycles(void) {
    u64 f;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
    return f * 10;
}
#define SPIN_LOCK_STUCK_CYCLES spin_stuck_cycles()
#else
#define SPIN_LOCK_STUCK_CYCLES 0ULL
static inline u64 spin_rdtsc(void) { return 0; }
#endif

static inline void spin_lock_at(spinlock_t *lock, u64 site) {
    int self = spin_owner_self();
    u64 deadline = 0;

    for (;;) {
        int seen = 0;

        /* Compare-and-swap, not exchange: an exchange writes the waiter's
         * token over the holder's on every failed attempt. */
        if (__atomic_compare_exchange_n(lock, &seen, self, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            break;
        if (__builtin_expect(seen == self && self != SPIN_OWNER_NOTASK, 0))
            spin_lock_recursive(lock, (u64)(usize)__builtin_return_address(0));
        /* Pause to hint to the CPU that we're in a spin-wait loop.
         * Improves performance and power consumption on SMP. */
#if defined(__x86_64__)
        __asm__ volatile("pause");
        tlb_shootdown_poll();
#elif defined(__aarch64__)
        __asm__ volatile("yield");
#endif
        /* A spin that never ends is a deadlock, not contention: turn the silent
         * hang into a named panic (which lock, which CPU, which task).
         *
         * "Never ends" has to mean the lock is never released. Seeing it free
         * — even though someone else won the race for it — proves the holder
         * is making progress, so the clock starts again. Without that rule the
         * check fired on a busy lock: the deadline is wall time, and a vCPU
         * descheduled by the host comes back with the whole deadline elapsed
         * and the lock long since free. Those reports all read `value=0`, a
         * lockup on a lock nobody held. */
        if (*lock == 0) {
            deadline = 0;
            continue;
        }
        if (deadline == 0)
            deadline = spin_rdtsc() + SPIN_LOCK_STUCK_CYCLES;
        else if (spin_rdtsc() > deadline)
            spin_lock_stuck(lock, (u64)(usize)__builtin_return_address(0));
    }
    if (site)
        spin_note_at(lock, SPIN_NOTE_ACQUIRE, site);
    else
        spin_note(lock, SPIN_NOTE_ACQUIRE);
    /* Held now. Under LOCKDEP this records who to blame when another CPU spins
     * on it; in the default build it compiles to nothing. */
    LOCKDEP_NOTE_SPIN_ACQUIRE(lock, (u64)(usize)__builtin_return_address(0));
}

static inline void spin_lock(spinlock_t *lock) {
    spin_lock_at(lock, 0);
}

static inline void spin_unlock(spinlock_t *lock) {
    int held = *lock;

    /* Writing 0 over a lock that is already 0 would hide a second unlock --
     * and the next one would then release somebody else's acquire. */
    if (__builtin_expect(held == 0, 0))
        spin_unlock_unheld(lock, (u64)(usize)__builtin_return_address(0));
    {
        int self = spin_owner_self();

        if (__builtin_expect(held != self && held != SPIN_OWNER_NOTASK &&
                                 self != SPIN_OWNER_NOTASK, 0))
            spin_unlock_foreign(lock, self,
                                (u64)(usize)__builtin_return_address(0));
    }
    /* Drop the holder record before the lock itself, so no window exists in
     * which the lock is free but still attributed to this CPU. */
    LOCKDEP_NOTE_SPIN_RELEASE(lock);
    spin_note(lock, SPIN_NOTE_RELEASE);
    /* Store 0 with a release barrier so all previous writes are visible
     * before the lock is released. A compiler-only barrier is insufficient
     * on AArch64: another CPU may observe the unlocked word before the
     * runqueue links (or other protected state) have become visible. */
#if defined(__aarch64__)
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
#else
    __asm__ volatile("" : : : "memory");
    *lock = 0;
#endif
}

/* One attempt, no spinning: 1 if this CPU now holds the lock, 0 if somebody
 * else does. For callers that must not wait on a holder who may never release
 * -- the panic path, which needs the console but cannot afford to hang on a
 * CPU that died holding it. */
static inline int spin_trylock(spinlock_t *lock) {
    int self = spin_owner_self();
    int seen = 0;

    if (!__atomic_compare_exchange_n(lock, &seen, self, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        /* These locks do not recurse, so trying one already held is a bug
         * whatever the answer would be (FreeBSD's mtx_trylock asserts it). */
        if (__builtin_expect(seen == self && self != SPIN_OWNER_NOTASK, 0))
            spin_lock_recursive(lock, (u64)(usize)__builtin_return_address(0));
        return 0;
    }
    spin_note(lock, SPIN_NOTE_TRY);
    LOCKDEP_NOTE_SPIN_ACQUIRE(lock, (u64)(usize)__builtin_return_address(0));
    return 1;
}

static inline int spin_trylock_irqsave(spinlock_t *lock, u64 *flags) {
    u64 saved;
#ifdef __x86_64__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved) : : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(saved) : : "memory");
#else
    u32 f32;
    __asm__ volatile("pushfd; popl %0; cli" : "=r"(f32) : : "memory");
    saved = f32;
#endif
    if (spin_trylock(lock)) {
        *flags = saved;
        if (__builtin_expect(kprof_irqoff_on, 0) && KPROF_IRQ_WAS_ON(saved))
            kprof_irqoff_begin(__builtin_return_address(0));
        return 1;
    }
    /* Not taken: restore the caller's interrupt state rather than leaving it
     * masked on a failed attempt. */
#ifdef __x86_64__
    if (saved & 0x200ULL)
        __asm__ volatile("sti" : : : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("msr daif, %0" : : "r"(saved) : "memory");
#endif
    return 0;
}

static inline int spin_is_locked(spinlock_t *lock) {
    return *lock != 0;
}

/* mtx_assert(MA_OWNED): the calling task holds `lock`. For the *_locked
 * helpers whose contract is "the caller holds X". */
void spin_assert_held_failed(volatile int *lock, u64 caller)
    __attribute__((noreturn));
static inline void spin_assert_held(spinlock_t *lock) {
    int self = spin_owner_self();

    if (__builtin_expect(*lock != self && self != SPIN_OWNER_NOTASK &&
                             *lock != SPIN_OWNER_NOTASK, 0))
        spin_assert_held_failed(lock, (u64)(usize)__builtin_return_address(0));
}

/* IRQ-safe variants (save/restore interrupt flag). The acquire loop is in
 * spin_lock, which polls TLB shootdowns — so an IRQs-off waiter here still
 * drains them and cannot deadlock the initiator. */
static inline void spin_lock_irqsave(spinlock_t *lock, u64 *flags) {
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
    spin_lock(lock);
}

/* spin_lock_irqsave on behalf of `site` (see spin_note_at). */
static inline void spin_lock_irqsave_at(spinlock_t *lock, u64 *flags, u64 site) {
#ifdef __x86_64__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags) : : "memory");
#elif defined(__aarch64__)
    u64 daif;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(daif) : : "memory");
    *flags = daif;
#endif
    spin_lock_at(lock, site);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    spin_unlock(lock);
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

#endif /* B1NIX_SPINLOCK_H */
