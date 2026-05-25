#ifndef B1NIX_LAPIC_H
#define B1NIX_LAPIC_H

#include <b1nix/types.h>

/* Forward declaration (defined in sched.h) */
struct task;

/* Per-CPU runqueue — linked list of READY tasks (SMP) */
struct runqueue {
    struct task *head;
    struct task *tail;
};

/* Local APIC MMIO registers (offsets from LAPIC base) */
#define LAPIC_ID                 0x020
#define LAPIC_VER                0x030
#define LAPIC_TPR                0x080
#define LAPIC_APR                0x090
#define LAPIC_PPR                0x0A0
#define LAPIC_EOI                0x0B0
#define LAPIC_RRD                0x0C0
#define LAPIC_LDR                0x0D0
#define LAPIC_DFR                0x0E0
#define LAPIC_SVR                0x0F0
#define LAPIC_ISR_BASE           0x100
#define LAPIC_TMR_BASE           0x180
#define LAPIC_IRR_BASE           0x200
#define LAPIC_ESR                0x280
#define LAPIC_ICR_LOW            0x300
#define LAPIC_ICR_HIGH           0x310
#define LAPIC_LVT_TIMER          0x320
#define LAPIC_LVT_THERMAL        0x330
#define LAPIC_LVT_PERFMON        0x340
#define LAPIC_LVT_LINT0          0x350
#define LAPIC_LVT_LINT1          0x360
#define LAPIC_LVT_ERROR          0x370
#define LAPIC_TIMER_INITCNT      0x380
#define LAPIC_TIMER_CURCNT       0x390
#define LAPIC_TIMER_DIV          0x3E0

/* LAPIC SVR flags */
#define LAPIC_SVR_ENABLE         (1 << 8)
#define LAPIC_SVR_FOCUS_DISABLE  (1 << 9)

/* LAPIC LVT flags */
#define LAPIC_LVT_MASKED         (1 << 16)
#define LAPIC_LVT_PERIODIC       (1 << 17)
#define LAPIC_LVT_ONESHOT        (0 << 17)
#define LAPIC_LVT_TSC_PERIODIC   (1 << 18)  /* TSC deadline mode */

/* ICR flags */
#define LAPIC_ICR_FIXED          0
#define LAPIC_ICR_LOWEST         1
#define LAPIC_ICR_SMI            (2 << 8)
#define LAPIC_ICR_NMI            (4 << 8)
#define LAPIC_ICR_INIT           (5 << 8)
#define LAPIC_ICR_STARTUP        (6 << 8)
#define LAPIC_ICR_LEVEL_DEASSERT (1 << 14)
#define LAPIC_ICR_LEVEL_ASSERT   (0 << 14)
#define LAPIC_ICR_TRIGGER_LEVEL  (1 << 15)
#define LAPIC_ICR_DEST_FIELD     (0 << 18)
#define LAPIC_ICR_DEST_SELF      (1 << 18)
#define LAPIC_ICR_DEST_ALL       (2 << 18)
#define LAPIC_ICR_DEST_OTHERS    (3 << 18)

/* LAPIC timer divide values */
#define LAPIC_TIMER_DIV_1        0x0B
#define LAPIC_TIMER_DIV_2        0x00
#define LAPIC_TIMER_DIV_4        0x01
#define LAPIC_TIMER_DIV_8        0x02
#define LAPIC_TIMER_DIV_16       0x03
#define LAPIC_TIMER_DIV_32       0x08
#define LAPIC_TIMER_DIV_64       0x09
#define LAPIC_TIMER_DIV_128      0x0A

/* MSRs */
#define IA32_APIC_BASE_MSR       0x1B
#define IA32_APIC_BASE_ENABLE    (1 << 11)
#define IA32_APIC_BASE_X2APIC    (1 << 10)
#define IA32_APIC_BASE_BSP       (1 << 8)

/* Spurious interrupt vector (must be >= 0x20) */
#define LAPIC_SPURIOUS_VECTOR    0xFF

/* APIC timer vector for scheduling */
#define LAPIC_TIMER_VECTOR       0x40

/* NMI IPI vector */
#define SMP_NMI_IPI_VECTOR       0x30

#define MAX_CPUS 16

/* Per-CPU data */
struct percpu {
    /* 0x00: CPU identification */
    u32 cpu_id;
    u32 apic_id;
    u32 cpu_online;

    /* 0x10: Current task */
    struct task *current_task;

    /* 0x18: Scheduler state */
    struct runqueue runqueue;
    u64 scheduler_ticks;
    int scheduler_started;

    /* 0x40: Kernel stack for this CPU (idle task stack) */
    u64 kernel_stack_phys;
    u64 kernel_stack_virt;
    u8 __pad[3840];  /* pad to 4KB total */
} __attribute__((aligned(4096)));

/* GS segment base management */
void arch_set_gs_base(u64 base);
u64 arch_get_gs_base(void);

/* Per-CPU access via GS segment.
 * Returns NULL if not yet initialized (GS base == 0). */
static inline struct percpu *get_percpu(void) {
    u64 gs = arch_get_gs_base();
    return gs ? (struct percpu *)gs : (struct percpu *)0;
}
#define percpu_write(member, val)   do { struct percpu *_p = get_percpu(); if (_p) _p->member = (val); } while(0)
#define percpu_read(member)         ({ struct percpu *_p = get_percpu(); _p ? _p->member : 0; })
#define percpu_ptr(member)          ({ struct percpu *_p = get_percpu(); _p ? &_p->member : (typeof(&_p->member))0; })

/* Current task accessor (SMP-safe via per-CPU) */
#define smp_current_task()          percpu_read(current_task)
#define smp_set_current_task(t)     percpu_write(current_task, (t))

/* APIC / SMP API */
void lapic_init(void);
void lapic_eoi(void);
void lapic_timer_start(u32 init_count);
u32 lapic_read(u32 reg);
void lapic_write(u32 reg, u32 val);
u32 lapic_id(void);
void lapic_send_ipi(u32 apic_id, u32 icr_low);
void lapic_send_ipi_allbutself(u32 icr_low);

/* AP bringup */
int smp_boot_aps(void);

/* Per-CPU init */
void percpu_init(void);

/* AP entry point (called from trampoline) */
void ap_main(u32 cpu_id);

#endif /* B1NIX_LAPIC_H */
