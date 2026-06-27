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
static volatile u32    g_tlb_vaddr   = 0;
static volatile int    g_tlb_pending = 0;

static volatile u64    g_tlb_gen     = 0;
static volatile u64    g_tlb_acked_gen[MAX_CPUS] = {0};

static volatile int    g_tlb_enabled = 0;

void tlb_shootdown_set_enabled(int enabled) {
    __atomic_store_n(&g_tlb_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
}

static inline int online_cpu_count(void) {
    int n = 0;
    for (int i = 0; i < g_max_cpus && i < MAX_CPUS; i++) {
        n++;
    }
    return n;
}

static inline void invlpg(u32 va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static inline void cr3_reload(void) {
    u32 cr3;
    __asm__ volatile("movl %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("movl %0, %%cr3" : : "r"(cr3) : "memory");
}

static inline int tlb_this_cpu(void) {
    struct percpu *p = get_percpu();
    return p ? (int)p->cpu_id : -1;
}

static void tlb_service_current(void) {
    int op = __atomic_load_n(&g_tlb_op, __ATOMIC_ACQUIRE);
    if (op == TLB_OP_NONE)
        return;
    int cpu = tlb_this_cpu();
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;
    u64 gen = __atomic_load_n(&g_tlb_gen, __ATOMIC_ACQUIRE);

    u64 expected = __atomic_load_n(&g_tlb_acked_gen[cpu], __ATOMIC_RELAXED);
    if (expected == gen)
        return;
    if (!__atomic_compare_exchange_n(&g_tlb_acked_gen[cpu], &expected, gen, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;

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

void tlb_shootdown_poll(void) {
    if (__atomic_load_n(&g_tlb_pending, __ATOMIC_ACQUIRE) <= 0)
        return;
    tlb_service_current();
}

static void tlb_shootdown_dispatch(int op, u32 vaddr) {
    int others = online_cpu_count() - 1;
    if (others <= 0) {
        return;
    }

    __atomic_add_fetch(&g_tlb_gen, 1, __ATOMIC_RELAXED);
    g_tlb_vaddr = vaddr;
    __atomic_store_n(&g_tlb_pending, others, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tlb_op, op, __ATOMIC_RELEASE);

    lapic_send_ipi_allbutself(TLB_SHOOTDOWN_VECTOR | LAPIC_ICR_FIXED);

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
    tlb_shootdown_dispatch(TLB_OP_PAGE, (u32)vaddr);
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

void ipi_reschedule_all(void) {
    if (g_max_cpus <= 1) return;
    lapic_send_ipi_allbutself(RESCHEDULE_VECTOR | LAPIC_ICR_FIXED);
}
