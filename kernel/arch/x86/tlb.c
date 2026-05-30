/* TLB shootdown (M28 #5). See kernel/include/b1nix/tlb.h.
 *
 * Design: a single global shootdown spinlock serialises requests, a small
 * descriptor records the operation, and a pending counter atomically tracks
 * how many target CPUs still need to ACK. Linux uses a per-CPU descriptor
 * for batching; b1nix doesn't yet have a workload that needs that.
 *
 * The initiator path:
 *   1. acquire g_tlb_lock + irqsave
 *   2. publish op + vaddr + pending = (online CPUs - 1)
 *   3. send IPI to all-but-self
 *   4. spin on pending == 0 (with a runaway-guard panic)
 *   5. release g_tlb_lock + irqrestore
 *
 * The target path (tlb_shootdown_handler, called from x86_irq_handler_inner):
 *   1. read the published op
 *   2. invlpg or cr3 reload
 *   3. atomic decrement pending
 *   4. lapic_eoi
 */

#include <b1nix/console.h>
#include <b1nix/ipi.h>
#include <b1nix/lapic.h>
#include <b1nix/panic.h>
#include <b1nix/spinlock.h>
#include <b1nix/tlb.h>

enum tlb_op {
    TLB_OP_NONE = 0,
    TLB_OP_PAGE = 1,
    TLB_OP_ALL  = 2,
};

static spinlock_t      g_tlb_lock    = SPINLOCK_INIT;
static volatile int    g_tlb_op      = TLB_OP_NONE;
static volatile u64    g_tlb_vaddr   = 0;
static volatile int    g_tlb_pending = 0;

/* Off until BKL goes away in M28 #7. See header for the rationale. */
static volatile int    g_tlb_enabled = 0;

void tlb_shootdown_set_enabled(int enabled) {
    __atomic_store_n(&g_tlb_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
}

static inline int online_cpu_count(void) {
    int n = 0;
    for (int i = 0; i < g_max_cpus && i < MAX_CPUS; i++) {
        struct percpu *p = (struct percpu *)0;
        /* Fast path: rely on the published g_max_cpus value from smp_boot_aps.
         * It already filters down to the actual count, so every index < that
         * is online. percpu_init runs synchronously for each AP before the
         * APs leave their bring-up loop, so this is conservative. */
        (void)p;
        n++;
    }
    return n;
}

static inline void invlpg(u64 va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static inline void cr3_reload(void) {
    u64 cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
}

void tlb_shootdown_handler(void) {
    /* Read the published op before any side effect.
     * Acquire-load pairs with the initiator's release store on pending. */
    int op = __atomic_load_n(&g_tlb_op, __ATOMIC_ACQUIRE);
    u64 va = g_tlb_vaddr;

    if (op == TLB_OP_PAGE) {
        invlpg(va);
    } else if (op == TLB_OP_ALL) {
        cr3_reload();
    }

    /* ACK by decrementing the pending counter. The initiator polls this. */
    __atomic_sub_fetch(&g_tlb_pending, 1, __ATOMIC_RELEASE);
    lapic_eoi();
}

/* Generic dispatch: publish op/vaddr + pending, send IPI to all-but-self,
 * wait for ACKs. Caller must hold g_tlb_lock + IRQs disabled. */
static void tlb_shootdown_dispatch(int op, u64 vaddr) {
    int others = online_cpu_count() - 1;
    if (others <= 0) {
        /* Single CPU — nothing to shoot down. */
        return;
    }

    g_tlb_vaddr = vaddr;
    /* The pending counter must be visible to handlers BEFORE the IPI lands,
     * and the op publish must precede pending. Strict ordering via release. */
    __atomic_store_n(&g_tlb_pending, others, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tlb_op, op, __ATOMIC_RELEASE);

    /* Fire the IPI. lapic_send_ipi_allbutself targets every LAPIC except this
     * one, with vector TLB_SHOOTDOWN_VECTOR + FIXED delivery. */
    lapic_send_ipi_allbutself(TLB_SHOOTDOWN_VECTOR | LAPIC_ICR_FIXED);

    /* Wait for ACKs with a generous runaway guard. A stuck target means a CPU
     * has IRQs disabled forever — a real bug we want to surface, not hide. */
    u64 spins = 0;
    while (__atomic_load_n(&g_tlb_pending, __ATOMIC_ACQUIRE) > 0) {
        __asm__ volatile("pause");
        if (++spins > (1ULL << 28)) {
            console_write("tlb: shootdown stalled, pending=");
            console_write_dec((u32)__atomic_load_n(&g_tlb_pending,
                                                  __ATOMIC_RELAXED));
            console_write(" op=");
            console_write_dec((u32)op);
            console_write("\n");
            panic("tlb_shootdown timeout");
        }
    }

    __atomic_store_n(&g_tlb_op, TLB_OP_NONE, __ATOMIC_RELAXED);
}

void tlb_shootdown_page(u64 vaddr) {
    if (g_max_cpus <= 1) return;
    if (!__atomic_load_n(&g_tlb_enabled, __ATOMIC_ACQUIRE)) return;
    u64 flags;
    spin_lock_irqsave(&g_tlb_lock, &flags);
    tlb_shootdown_dispatch(TLB_OP_PAGE, vaddr);
    spin_unlock_irqrestore(&g_tlb_lock, flags);
}

void tlb_shootdown_all(void) {
    if (g_max_cpus <= 1) return;
    if (!__atomic_load_n(&g_tlb_enabled, __ATOMIC_ACQUIRE)) return;
    u64 flags;
    spin_lock_irqsave(&g_tlb_lock, &flags);
    tlb_shootdown_dispatch(TLB_OP_ALL, 0);
    spin_unlock_irqrestore(&g_tlb_lock, flags);
}

/* M28 #6: reschedule IPI sender. No state to publish — the handler is just
 * lapic_eoi (see x86_irq_handler vector-66 branch). Fire-and-forget. */
void ipi_reschedule_all(void) {
    if (g_max_cpus <= 1) return;
    lapic_send_ipi_allbutself(RESCHEDULE_VECTOR | LAPIC_ICR_FIXED);
}
