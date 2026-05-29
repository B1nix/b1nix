#include <b1nix/bkl.h>
#include <b1nix/lapic.h>
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
        while (g_bkl)
            __asm__ volatile("pause");
        u64 fl = irq_save_cli();
        if (spin_xchg(&g_bkl, 1) == 0) {
            g_bkl_owner = cpu;
            g_bkl_depth = 1;
            irq_restore(fl);
            return;
        }
        irq_restore(fl);
    }
}

void bkl_unlock(void) {
    u64 fl = irq_save_cli();
    if (g_bkl_depth > 0 && --g_bkl_depth == 0) {
        g_bkl_owner = -1;          /* clear owner before releasing (x86 TSO keeps
                                    * this store ordered ahead of the unlock) */
        __asm__ volatile("" : : : "memory");
        g_bkl = 0;                 /* release */
    }
    irq_restore(fl);
}
