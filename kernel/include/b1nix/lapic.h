#ifndef B1NIX_LAPIC_H
#define B1NIX_LAPIC_H

#include <b1nix/types.h>
#include <b1nix/spinlock.h>

/* Forward declaration (defined in sched.h) */
struct task;

/* Per-CPU runqueue — linked list of READY tasks (SMP) */
struct runqueue {
    spinlock_t  lock; /* protects head/tail under SMP */
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

/* TLB shootdown IPI vector (M28 #5). Targets invalidate the requested vaddr
 * range from their local TLB then decrement the pending counter. */
#define TLB_SHOOTDOWN_VECTOR     0x41

/* Reschedule IPI (M28 #6). A no-op handler whose sole purpose is to wake a
 * CPU out of `sti; hlt` in its idle loop so it can re-poll the global
 * runqueue. EOI inside the handler; no other state. */
#define RESCHEDULE_VECTOR        0x42

/* NMI IPI vector */
#define SMP_NMI_IPI_VECTOR       0x30

/* Compile-time ceiling for per-CPU data (TSS array, AP pointer table, idle
 * task slot table). Runtime CPU count is g_max_cpus (set from ACPI by
 * smp_boot_aps) — most loops should bound by that. MAX_CPUS was 16 prior
 * to the C3 audit pass; raised to 64 so multi-socket x86_64 boxes aren't
 * cut off at the desktop-tier limit. Static cost: x86_tss_arr ~7 KiB,
 * ap_cpu_data 512 B; idle tasks are heap-allocated lazily per AP. */
#define MAX_CPUS 64

/* Runtime CPU count: 1 until smp_boot_aps observes ACPI / CPUID and
 * publishes the discovered total. Code that walks CPUs should prefer this
 * to the MAX_CPUS ceiling. */
extern int g_max_cpus;

/* Per-CPU data */
struct percpu {
    /* 0x00: CPU identification */
    u32 cpu_id;
    u32 apic_id;
    u32 cpu_online;
#ifndef __x86_64__
    u32 self_ptr;
#endif

    /* 0x10: Current task. Named cur_task (not current_task) so it does not
     * collide with the `current_task` macro (#define current_task
     * get_percpu()->cur_task in sched.h). syscall_entry.S reads it as %gs:0x10. */
    struct task *cur_task;
#ifndef __x86_64__
    u32 _pad_cur_task;
#endif

    /* 0x18: Scheduler state */
    struct runqueue runqueue;
    u64 scheduler_ticks;
    int scheduler_started;

    /* 0x40: Kernel stack for this CPU (idle task stack) */
    u64 kernel_stack_phys;
    u64 kernel_stack_virt;

    /* SMP work-stealing: when an AP switches into a stolen worker, it stores a
     * pointer to its own idle struct cpu_context here so the worker can switch
     * back to the AP idle loop when done (kept as void* to avoid pulling
     * sched.h into this header). */
    void *sched_return_ctx;

    /* Per-CPU idle task (struct task *). Set on APs so the cooperative scheduler
     * can park back to it when no other task is runnable; NULL on the BSP (whose
     * boot task serves that role). void* to avoid pulling sched.h here. */
    void *idle_task;

    /* 0x60: SYSCALL entry scratch. syscall_entry.S stores the incoming RAX
     * here before it reuses RAX to load cur_task. This must be per-CPU:
     * a single global scratch races before the BKL can be acquired. */
    u64 syscall_scratch_rax;

    u8 __pad[3816];  /* pad to 4KB total */
} __attribute__((aligned(4096)));

/* Segment base management */
#ifdef __x86_64__
void arch_set_gs_base(u64 base);
u64 arch_get_gs_base(void);
#else
void arch_set_fs_base_percpu(u32 base);
u32 arch_get_fs_base_percpu(void);
#endif

/* Per-CPU access.
 * Returns NULL if not yet initialized. */
static inline struct percpu *get_percpu(void) {
#ifdef __x86_64__
    u64 gs = arch_get_gs_base();
    return gs ? (struct percpu *)gs : (struct percpu *)0;
#else
    u32 fs = arch_get_fs_base_percpu();
    return fs ? (struct percpu *)fs : (struct percpu *)0;
#endif
}
#define percpu_write(member, val)   do { struct percpu *_p = get_percpu(); if (_p) _p->member = (val); } while(0)
#define percpu_read(member)         ({ struct percpu *_p = get_percpu(); _p ? _p->member : 0; })
#define percpu_ptr(member)          ({ struct percpu *_p = get_percpu(); _p ? &_p->member : (typeof(&_p->member))0; })

/* Current task accessor (SMP-safe via per-CPU) */
#define smp_current_task()          percpu_read(cur_task)
#define smp_set_current_task(t)     percpu_write(cur_task, (t))

/* APIC / SMP API */
void lapic_init(void);
/* Per-CPU LAPIC software-enable + LVT masking + TPR clear. lapic_init runs
 * this on the BSP transparently; each AP must call it from x86_ap_arch_init
 * before any interrupt (timer tick, IPI, etc.) can be delivered to that CPU.
 * Before this lands, an AP's LAPIC stays in its reset state (SVR.SoftEnable
 * = 0) so every locally-delivered vector — including the LAPIC timer tick
 * we armed in M28-A and the TLB shootdown IPI in M28 #5 — is silently
 * dropped. */
void lapic_init_local(void);
void lapic_eoi(void);
void lapic_timer_start(u32 init_count);
/* Arm the LAPIC timer in periodic mode at LAPIC_TIMER_VECTOR with a period of
 * `ms` milliseconds. Returns 1 on success, 0 if the timer is uncalibrated
 * (lapic_ticks_per_ms() == 0) or the requested cadence overflows the 32-bit
 * init count. Each CPU calls this once after its LAPIC is initialised. */
int lapic_timer_start_periodic_ms(u32 ms);
/* Non-zero once any CPU has armed the periodic LAPIC timer. main.c uses this
 * to decide whether it can safely mask PIT IRQ0 (the previous tick source). */
int lapic_timer_periodic_active(void);
u32 lapic_read(u32 reg);
void lapic_write(u32 reg, u32 val);
u32 lapic_id(void);
void lapic_send_ipi(u32 apic_id, u32 icr_low);
void lapic_send_ipi_allbutself(u32 icr_low);

/* LAPIC-timer frequency (ticks per millisecond at divide=16), calibrated
 * against the PIT at lapic_init. 0 until calibration runs. */
u32 lapic_ticks_per_ms(void);

/* AP bringup */
int smp_boot_aps(void);

/* Per-CPU init */
void percpu_init(void);

/* AP entry point (called from trampoline) */
void ap_main(u32 cpu_id);

/* Set by the BSP (main.c) once the SMP self-test finishes, telling APs to leave
 * the work-stealing-only loop and run the full cooperative scheduler (ordinary
 * userspace tasks) under the Big Kernel Lock. */
extern volatile int g_ap_userspace_enabled;

/* Per-CPU arch init for an Application Processor (kernel/arch/x86_64/arch.c):
 * loads the kernel GDT/IDT, this CPU's TSS, and the SYSCALL/SSE MSRs so the AP
 * can execute ring 3. */
void x86_ap_arch_init(int cpu);

/* SMP percpu accessors for task stealing */
struct percpu *get_percpu_n(int idx);   /* returns NULL if idx out of range or CPU offline */
int            get_online_cpu_count(void);

#endif /* B1NIX_LAPIC_H */
