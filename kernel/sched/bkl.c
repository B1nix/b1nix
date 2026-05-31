#include <b1nix/bkl.h>
#include <b1nix/lapic.h>
#include <b1nix/lockdep.h>
#include <b1nix/spinlock.h>

/* Underlying test-and-set lock plus owner/depth bookkeeping. owner == -1 means
 * unowned; otherwise it holds the cpu_id of the current owner. */
static spinlock_t  g_bkl       = SPINLOCK_INIT;
static volatile int g_bkl_owner = -1;
static volatile u32 g_bkl_depth = 0;

static inline int this_cpu_id(void) {
    struct percpu *p = get_percpu();
    return p ? (int)p->cpu_id : 0; /* pre-percpu boot is the BSP (cpu 0) */
}

static inline u64 irq_save_cli(void) {
    u64 fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) : : "memory");
    return fl;
}

static inline void irq_restore(u64 fl) {
    __asm__ volatile("pushq %0; popfq" : : "r"(fl) : "memory", "cc");
}

void bkl_lock(void) {
    int cpu = this_cpu_id();

    /* Fast path: we already own it (e.g. an interrupt nested inside our own
     * syscall). Only the owning CPU ever observes owner == its own id, so this
     * read needs no special ordering. */
    if (g_bkl_owner == cpu) {
        g_bkl_depth++;
        return;
    }

    /* Slow path. Interrupts must stay off from the moment we win the spinlock
     * until ownership is published: otherwise an interrupt arriving in that
     * window would call bkl_lock(), see owner != cpu, find the lock already
     * held (by us), and spin forever. */
    for (;;) {
        while (g_bkl) {
            __asm__ volatile("pause");
            /* This loop can run with interrupts disabled (callers in the
             * context-switch / AP idle paths enter under cli). A CPU spinning
             * here with IRQs off cannot take the TLB-shootdown IPI, which would
             * deadlock the initiator; drain shootdowns explicitly. See
             * tlb_shootdown_poll. */
            tlb_shootdown_poll();
        }
        u64 fl = irq_save_cli();
        if (spin_xchg(&g_bkl, 1) == 0) {
            g_bkl_owner = cpu;
            g_bkl_depth = 1;
            /* Lockdep tracks only the outermost acquire — recursive
             * re-entry by the same CPU doesn't bump the global counter
             * a second time. M24b's bequeath model lets the release CPU
             * differ from the acquire CPU (task migrates mid-syscall),
             * so this lock uses the GLOBAL singleton entry that bypasses
             * the per-CPU acquisition stack. See lockdep.h. */
            LOCKDEP_ACQUIRE_GLOBAL(LOCKDEP_LVL_BKL);
            irq_restore(fl);
            return;
        }
        irq_restore(fl);
    }
}

void bkl_unlock(void) {
    u64 fl = irq_save_cli();
    int cpu = this_cpu_id();
    /* T1 (M28 #7): no-op if this CPU isn't the owner. Two reasons:
     *  1. Idempotency for callsites being progressively removed (T1..T5).
     *     user_jump.S calls bkl_unlock unconditionally on a fresh task's
     *     first entry to ring 3; once we stop holding the BKL in the AP
     *     idle loop, that call must not corrupt another CPU's depth.
     *  2. Latent-bug fix: the old test g_bkl_depth > 0 looked at the
     *     OWNER's depth, so a non-owner calling unlock would decrement
     *     someone else's counter. Under M24b BKL this couldn't happen
     *     in practice — every release was preceded by a matching
     *     acquire on the same CPU — but the check is the right shape
     *     either way. */
    if (g_bkl_owner != cpu) {
        irq_restore(fl);
        return;
    }
    if (g_bkl_depth > 0 && --g_bkl_depth == 0) {
        g_bkl_owner = -1;          /* clear owner before releasing (x86 TSO keeps
                                    * this store ordered ahead of the unlock) */
        __asm__ volatile("" : : : "memory");
        g_bkl = 0;                 /* release */
        LOCKDEP_RELEASE_GLOBAL(LOCKDEP_LVL_BKL);
    }
    irq_restore(fl);
}

int bkl_is_held_by_current_cpu(void) {
    return g_bkl_owner == this_cpu_id();
}

u32 bkl_get_depth(void) {
    return g_bkl_depth;
}

void bkl_unlock_for_switch(void) {
    u64 fl = irq_save_cli();
    int cpu = this_cpu_id();
    if (g_bkl_owner == cpu) {
        g_bkl_depth = 0;
        g_bkl_owner = -1;
        __asm__ volatile("" : : : "memory");
        g_bkl = 0;
        LOCKDEP_RELEASE_GLOBAL(LOCKDEP_LVL_BKL);
    }
    irq_restore(fl);
}

void bkl_lock_for_switch(u32 depth) {
    int cpu = this_cpu_id();
    for (;;) {
        while (g_bkl) {
            __asm__ volatile("pause");
            tlb_shootdown_poll(); /* drain shootdowns while spinning IRQs-off */
        }
        u64 fl = irq_save_cli();
        if (spin_xchg(&g_bkl, 1) == 0) {
            g_bkl_owner = cpu;
            g_bkl_depth = depth;
            LOCKDEP_ACQUIRE_GLOBAL(LOCKDEP_LVL_BKL);
            irq_restore(fl);
            return;
        }
        irq_restore(fl);
    }
}
