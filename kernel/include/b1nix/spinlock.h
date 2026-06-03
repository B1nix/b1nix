#ifndef B1NIX_SPINLOCK_H
#define B1NIX_SPINLOCK_H

#include <b1nix/types.h>

/* Spinlock: simple ticket lock / test-and-set lock
 * For UP (single-core) builds the lock is a no-op.
 * For SMP builds it uses a real xchg-based spinlock.
 *
 * The lock is "locked" when the value is 1, "unlocked" when 0.
 */

typedef volatile int spinlock_t;

#define SPINLOCK_INIT 0

/* Atomically exchange byte: lock cmpxchg or xchg.
 * Returns the old value. */
static inline int spin_xchg(volatile int *lock, int val) {
    int old;
    __asm__ volatile("xchg %0, %1"
                     : "=r"(old), "+m"(*lock)
                     : "0"(val)
                     : "memory");
    return old;
}

/* Drain any in-flight cross-CPU TLB shootdown while spin-waiting (defined in
 * kernel/arch/x86_64/tlb.c). A CPU spinning here may have interrupts disabled (it
 * was called under cli, or via spin_lock_irqsave) and so cannot take the
 * shootdown IPI; the shootdown initiator also waits IRQs-off, so without this
 * poll the two deadlock. Fast path is a single load when nothing is pending, so
 * uniprocessor and uncontended SMP pay nothing (the loop body only runs when a
 * lock is actually contended). Always linked (tlb.c is in every kernel). */
void tlb_shootdown_poll(void);

static inline void spin_lock(spinlock_t *lock) {
    /* Spin until we successfully exchange 1 (locked) with the old value.
     * xchg is implicitly locked on x86 when used with a memory operand. */
    while (spin_xchg(lock, 1) != 0) {
        /* Pause to hint to the CPU that we're in a spin-wait loop.
         * Improves performance and power consumption on SMP. */
        __asm__ volatile("pause");
        tlb_shootdown_poll();
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    /* Store 0 with a release barrier so all previous writes are visible
     * before the lock is released. */
    __asm__ volatile("" : : : "memory");
    *lock = 0;
}

static inline int spin_is_locked(spinlock_t *lock) {
    return *lock != 0;
}

/* IRQ-safe variants (save/restore interrupt flag). The acquire loop is in
 * spin_lock, which polls TLB shootdowns — so an IRQs-off waiter here still
 * drains them and cannot deadlock the initiator. */
static inline void spin_lock_irqsave(spinlock_t *lock, u64 *flags) {
#ifdef __x86_64__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags) : : "memory");
#else
    u32 f32;
    __asm__ volatile("pushfd; popl %0; cli" : "=r"(f32) : : "memory");
    *flags = f32;
#endif
    spin_lock(lock);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, u64 flags) {
    spin_unlock(lock);
#ifdef __x86_64__
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory");
#else
    u32 f32 = (u32)flags;
    __asm__ volatile("pushl %0; popfd" : : "r"(f32) : "memory");
#endif
}

#endif /* B1NIX_SPINLOCK_H */
