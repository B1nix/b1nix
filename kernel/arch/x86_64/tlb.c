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

/* Per-shootdown generation + per-CPU "last generation this CPU has ACKed".
 *
 * A CPU that is spin-waiting on ANY spin_lock_irqsave lock has interrupts
 * disabled and therefore cannot take the shootdown IPI — so the initiator
 * (which spins on g_tlb_pending with IRQs off too) would wait forever. The fix
 * is tlb_shootdown_poll(), called from the irqsave spin loop, which lets a
 * waiter service the in-flight shootdown itself. Because a target can then ACK
 * via EITHER the poll OR the (still-pending) IPI, the decrement must be
 * idempotent per CPU per shootdown: g_tlb_acked_gen[cpu] gates it. The
 * initiator holds g_tlb_lock for the whole dispatch, so g_tlb_op/vaddr/gen are
 * stable while g_tlb_pending > 0 (no torn cross-generation reads while it
 * matters); a late IPI that fires after the next dispatch has begun simply
 * services that newer (also stable) generation, still exactly once. */
static volatile u64    g_tlb_gen     = 0;
static volatile u64    g_tlb_acked_gen[MAX_CPUS] = {0};

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

static inline int tlb_this_cpu(void) {
    struct percpu *p = get_percpu();
    return p ? (int)p->cpu_id : -1;
}

/* Apply the in-flight shootdown for THIS CPU, decrementing g_tlb_pending at
 * most once per generation. Safe to call from the IPI handler OR from a spin
 * waiter (poll). Same CPU never runs both concurrently (poll runs IRQs-off; the
 * IPI only fires IRQs-on), so g_tlb_acked_gen[cpu] needs no CAS — only this CPU
 * writes its slot. Returns with TLB flushed for the current generation. */
static void tlb_service_current(void) {
    int op = __atomic_load_n(&g_tlb_op, __ATOMIC_ACQUIRE);
    if (op == TLB_OP_NONE)
        return; /* nothing in flight (between rounds) */
    int cpu = tlb_this_cpu();
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;
    u64 gen = __atomic_load_n(&g_tlb_gen, __ATOMIC_ACQUIRE);

    /* Claim this generation for this CPU via CAS before doing the work. Only
     * this CPU writes its own slot, but the poll (plain spin_lock can run with
     * IRQs on) may be interrupted by the real shootdown IPI mid-service — the
     * CAS makes exactly one of {poll, IPI} the claimant, so g_tlb_pending is
     * decremented once. The loser returns without touching it; the winner (same
     * CPU) has already flushed, which is all this core needs. */
    u64 expected = __atomic_load_n(&g_tlb_acked_gen[cpu], __ATOMIC_RELAXED);
    if (expected == gen)
        return; /* already serviced this generation */
    if (!__atomic_compare_exchange_n(&g_tlb_acked_gen[cpu], &expected, gen, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return; /* a concurrent IPI/poll on this CPU claimed it first */

    /* op/vaddr/gen are stable here: the initiator holds g_tlb_lock until
     * g_tlb_pending hits 0, so this generation cannot be reused mid-service. */
    if (op == TLB_OP_PAGE)
        invlpg(g_tlb_vaddr);
    else if (op == TLB_OP_ALL)
        cr3_reload();

    __atomic_sub_fetch(&g_tlb_pending, 1, __ATOMIC_RELEASE);
}

void tlb_shootdown_handler(void) {
    tlb_service_current();
    lapic_eoi();
}

/* Called from the spin_lock_irqsave wait loop: a CPU spinning with IRQs
 * disabled drains any in-flight shootdown so the initiator can make progress.
 * Fast-paths to a single load when nothing is pending (the common case, and
 * always true on a single-CPU boot). */
void tlb_shootdown_poll(void) {
    if (__atomic_load_n(&g_tlb_pending, __ATOMIC_ACQUIRE) <= 0)
        return;
    tlb_service_current();
}

/* Generic dispatch: publish op/vaddr + pending, send IPI to all-but-self,
 * wait for ACKs. Caller must hold g_tlb_lock + IRQs disabled. */
static void tlb_shootdown_dispatch(int op, u64 vaddr) {
    int others = online_cpu_count() - 1;
    if (others <= 0) {
        /* Single CPU — nothing to shoot down. */
        return;
    }

    /* Open a new generation so targets (via IPI or poll) ACK it exactly once.
     * Bumped before the op release so a handler that observes op != NONE also
     * observes the matching generation. */
    __atomic_add_fetch(&g_tlb_gen, 1, __ATOMIC_RELAXED);
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
