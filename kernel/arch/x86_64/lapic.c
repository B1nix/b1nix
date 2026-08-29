#include <b1nix/arch.h>
#include <b1nix/lapic.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/io.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/runqueue.h>
#include <b1nix/acpi.h>
#include <b1nix/ktime.h>
#include <string.h>

/* Cleared at boot; the BSP sets it after the SMP self-test so APs leave the
 * work-stealing-only loop and run the full cooperative scheduler (userspace)
 * under the Big Kernel Lock. */
volatile int g_ap_userspace_enabled = 0;

/* Total online CPUs after smp_boot_aps. Visible everywhere via <b1nix/lapic.h>
 * so loop bounds can shrink from MAX_CPUS (ceiling) to the actual count. */
int g_max_cpus = 1;

/* External functions */
extern void arch_context_switch(struct cpu_context *old, struct cpu_context *new,
                                volatile int *released_publish);
extern void paging_switch_address_space(u64 pml4_phys);
extern void arch_set_kernel_stack(u64 stack);

/* Forward decl — defined below; called from lapic_init. */
static void apic_timer_calibrate_against_pit(void);

/* Map LAPIC MMIO at a fixed virtual address in kernel space.
 * We use 0xFFFFFE0000000000 which is in the kernel's high mapping area
 * and unlikely to collide with anything. */
#define LAPIC_VIRT_BASE  ((volatile u32 *)0xFFFFFE0000000000ULL)

static volatile u32 *lapic_base = 0;

static void lapic_map_base(u64 phys_base) {
    /* LAPIC MMIO region is 4KB */
    u64 virt = (u64)LAPIC_VIRT_BASE;
    for (u64 offset = 0; offset < 0x1000; offset += 0x1000) {
        vmm_map_page(virt + offset, phys_base + offset, VMM_PRESENT | VMM_WRITABLE);
    }
    lapic_base = LAPIC_VIRT_BASE;
}

u32 lapic_read(u32 reg) {
    if (!lapic_base) return 0;
    return lapic_base[reg / 4];
}

void lapic_write(u32 reg, u32 val) {
    if (!lapic_base) return;
    lapic_base[reg / 4] = val;
}

u32 lapic_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

void lapic_timer_start(u32 init_count) {
    /* Set divide configuration to 1 (no division) */
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_1);
    /* Set LVT timer entry: periodic mode, vector LAPIC_TIMER_VECTOR */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC);
    /* Set initial count */
    lapic_write(LAPIC_TIMER_INITCNT, init_count);
}

static volatile int g_lapic_timer_periodic_active = 0;

/* The period the scheduler tick is actually programmed with, in milliseconds.
 *
 * The tick rate used to be a constant repeated in four headers, and every
 * conversion between time and ticks trusted that the timer had been programmed
 * to match. It is the timer that decides: this records what was asked of the
 * hardware, and sched_tick_hz() below is what the rest of the kernel reads.
 * Zero until a periodic timer is armed. */
static u32 g_tick_period_ms;

/* Ticks per second, as programmed.
 *
 * The fallback is the PIT rate the boot path leaves running when the LAPIC
 * timer cannot be calibrated (see TIMER_HZ in interrupts.c) — the one place a
 * number still has to be assumed, because at that point nothing has programmed
 * a rate to read back. */
u32 sched_tick_hz(void) {
    return g_tick_period_ms ? 1000u / g_tick_period_ms : 100u;
}

int lapic_timer_start_periodic_ms(u32 ms) {
    /* PIT-calibrated rate (per ms at divide=16). Zero means lapic_init never
     * managed to calibrate — without that we have no idea what one tick is, so
     * refuse rather than program a bogus cadence. The caller (main.c) treats a
     * 0 return as "leave PIT IRQ0 alive". */
    u32 tpms = lapic_ticks_per_ms();
    if (tpms == 0 || ms == 0) return 0;

    /* Detect overflow before storing a wrapped value. The 32-bit LAPIC init
     * count holds at most ~68 s at QEMU's ~62 kticks/ms — far beyond the 10 ms
     * scheduler tick we use today, but the guard keeps a future caller from
     * silently programming a 0-init-count (= timer disabled). */
    u64 init64 = (u64)tpms * (u64)ms;
    if (init64 == 0 || init64 > 0xFFFFFFFFULL) return 0;

    /* Match the calibration divider so the rate computed by
     * apic_timer_calibrate_against_pit lines up with what we program here. */
    g_tick_period_ms = ms;
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INITCNT, (u32)init64);

    g_lapic_timer_periodic_active = 1;
    return 1;
}

int lapic_timer_periodic_active(void) { return g_lapic_timer_periodic_active; }

/* Waits for the local ICR to go idle (delivery status clear) before issuing a
 * new IPI. Drains pending TLB shootdowns while spinning: this CPU may be
 * running with interrupts disabled, so the shootdown IPI it owes another CPU
 * can only be serviced by polling. */
static void lapic_icr_wait_idle(void) {
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12)) {
        __asm__ volatile("pause");
        tlb_shootdown_poll();
    }
}

void lapic_send_ipi(u32 apic_id, u32 icr_low) {
    lapic_icr_wait_idle();
    /* xAPIC (MMIO) mode: the destination APIC ID lives in ICR_HIGH bits
     * [31:24]. The old `(u64)apic_id << 32` is the x2APIC (MSR) layout; written
     * through the u32 lapic_write it truncated to 0, so every IPI targeted APIC
     * 0 (the BSP) — an INIT to self triple-faults the boot CPU. */
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    /* Write command to ICR low */
    lapic_write(LAPIC_ICR_LOW, icr_low);
    /* Wait for delivery to complete */
    lapic_icr_wait_idle();
}

void lapic_send_ipi_allbutself(u32 icr_low) {
    lapic_icr_wait_idle();
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, icr_low | LAPIC_ICR_DEST_OTHERS);
    lapic_icr_wait_idle();
}

/* Fire-and-forget all-but-self IPI: never waits for delivery.
 *
 * A fixed IPI is only accepted by a target whose IRR bit for that vector is
 * clear; while the target has interrupts disabled the bit stays set, so a
 * second IPI on the same vector leaves delivery status pending indefinitely.
 * Two CPUs doing that to each other — both spinning on delivery status with
 * IRQs off — deadlock permanently (observed with the reschedule vector: both
 * CPUs wedged in scheduler_wake_all -> ipi_reschedule_all). For a kick-style
 * IPI with no payload, a delivery already pending on the target does the job
 * just as well as a fresh one, so skip the send instead of waiting. */
void lapic_send_ipi_allbutself_nowait(u32 icr_low) {
    if (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
        return; /* previous IPI not accepted yet — this one is redundant */
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, icr_low | LAPIC_ICR_DEST_OTHERS);
}

void lapic_init_local(void) {
    /* Software enable LAPIC via SVR — the CPU otherwise drops every locally
     * delivered interrupt (timer, IPI). Spurious vector goes in the low byte. */
    u32 svr = lapic_read(LAPIC_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr &= ~LAPIC_SVR_FOCUS_DISABLE;
    svr = (svr & ~0xFF) | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    /* Mask unused LVT entries (not LINT0/LINT1 — those carry PIC interrupts). */
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERFMON, LAPIC_LVT_MASKED);

    /* TPR = 0 so every vector >= 0x10 is accepted. */
    lapic_write(LAPIC_TPR, 0);
}

void lapic_init(void) {
    /* Check CPUID for APIC presence */
    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (!(edx & (1 << 9))) {
        console_write("lapic: APIC not supported by CPU\n");
        return;
    }

    /* Read APIC base from MSR */
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(IA32_APIC_BASE_MSR));
    u64 apic_base_phys = ((u64)hi << 32) | lo;
    
    if (!(apic_base_phys & IA32_APIC_BASE_ENABLE)) {
        console_write("lapic: APIC not enabled, enabling\n");
        apic_base_phys |= IA32_APIC_BASE_ENABLE;
        lo = (u32)(apic_base_phys & 0xFFFFFFFF);
        hi = (u32)(apic_base_phys >> 32);
        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(IA32_APIC_BASE_MSR));
    }

    u64 phys_base = apic_base_phys & ~0xFFFULL;
    console_write("lapic: base at 0x");
    console_write_hex64(phys_base);
    console_write("\n");

    /* Map LAPIC MMIO */
    lapic_map_base(phys_base);

    /* Read LAPIC version */
    u32 ver = lapic_read(LAPIC_VER);
    u32 max_lvt = (ver >> 16) & 0xFF;
    console_write("lapic: version=0x");
    console_write_hex64(ver);
    console_write(" max_lvt=");
    console_write_dec(max_lvt);
    console_write("\n");

    /* Per-CPU LAPIC state — factored into lapic_init_local so APs can run
     * the same setup from x86_ap_arch_init. Before this factoring, AP LAPICs
     * stayed software-disabled and every locally-delivered vector (timer,
     * IPI) was silently dropped — invisible until M28 #5's TLB shootdown
     * actually needed the IPI to arrive. */
    lapic_init_local();

    console_write("lapic: initialized (id=");
    console_write_dec(lapic_id());
    console_write(")\n");

    /* Calibrate the LAPIC timer against the PIT so the value is available
     * before any subsystem requests a LAPIC-driven tick. Requires interrupts
     * disabled at the source — PIT channel 2 doesn't fire IRQ0, and the
     * LVT_TIMER entry stays masked during calibration. */
    apic_timer_calibrate_against_pit();
}

/* ── Per-CPU data ── */

static struct percpu boot_cpu_data = {
    .cpu_id = 0,
    .apic_id = 0,
    .cur_task = 0,
    .scheduler_ticks = 0,
    .scheduler_started = 0,
    /* .runqueue.lock = 0 — zero-initialized by static storage */
};

void arch_set_gs_base(u64 base) {
    u64 lo = base & 0xFFFFFFFF;
    u64 hi = (base >> 32) & 0xFFFFFFFF;
    /* WRMSR to IA32_GS_BASE (0xC0000101) */
    __asm__ volatile("wrmsr" : : "c"(0xC0000101), "a"(lo), "d"(hi));
}

u64 arch_get_gs_base(void) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    return ((u64)hi << 32) | lo;
}

void percpu_init(void) {
    /* Set BSP per-CPU data */
    boot_cpu_data.apic_id = 0;
    boot_cpu_data.cpu_online = 1;  /* BSP is online; lets APs see it as a steal victim */
    arch_set_gs_base((u64)&boot_cpu_data);

    /* If APIC is available, get real APIC ID */
    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (edx & (1 << 9)) {
        /* EBX[31:24] contains initial APIC ID on modern CPUs */
        u32 apic_id = (ebx >> 24) & 0xFF;
        boot_cpu_data.apic_id = apic_id;
        boot_cpu_data.cpu_id = 0;
    }

    console_write("percpu: BSP initialized (cpu=0 apic_id=");
    console_write_dec(boot_cpu_data.apic_id);
    console_write(")\n");
}

/* ── AP boot trampoline ── */

/* Include AP trampoline binary (generated by Makefile) */
#include "ap_trampoline.inc"

/* Extern for kernel PML4 (defined in boot.S) */
extern u64 pml4[];

/* Per-CPU data for APs */
static struct percpu *ap_cpu_data[MAX_CPUS];

/* Read another CPU's current task (used by the TLB-shootdown timeout diagnostic
 * to name the stuck CPU). cpu 0 is the BSP (boot_cpu_data); APs live in
 * ap_cpu_data[]. Best-effort: no lock — the timeout path is already a panic. */
struct task *percpu_cur_task(int cpu) {
    if (cpu == 0) return (struct task *)boot_cpu_data.cur_task;
    if (cpu > 0 && cpu < MAX_CPUS && ap_cpu_data[cpu])
        return (struct task *)ap_cpu_data[cpu]->cur_task;
    return 0;
}

/* AP trampoline — flat binary linked at 0x8000.
 * Generated by kernel/arch/x86_64/ap_trampoline.S. */
extern u8 ap_trampoline_bin[];
extern u32 ap_trampoline_bin_len;

/* Offsets of the trampoline's data block, GENERATED from the same object the
 * blob is linked from (build/<arch>/inc/ap_trampoline_offsets.h, produced by
 * `nm`). They were hand-written constants until the trampoline's code grew and
 * pushed the block down: the BSP then wrote CR3, RSP and the ap_main pointer
 * into the middle of the trampoline's own code, and every AP triple-faulted the
 * machine on its first SIPI. Deriving them removes the duplicate knowledge that
 * made that possible; the magic word below is the runtime witness that the blob
 * and these offsets came from the same build. */
#include "ap_trampoline_offsets.h"
#define TRAMP_MAGIC       0x54524D50303031ULL /* "TRMP001" */

static void smp_setup_trampoline(u64 pml4_phys, u64 stack_virt,
                                  u64 percpu_ptr, u32 cpu_id)
{
    u64 tv = 0xFFFF800000000000ULL + 0x8000;
    usize tramp_len = (usize)ap_trampoline_bin_len;

    if (tramp_len > 0x1000) tramp_len = 0x1000;

    /* Copy trampoline code */
    for (usize i = 0; i < tramp_len; i++)
        *(volatile u8 *)(tv + i) = ap_trampoline_bin[i];

    /* The blob must be the trampoline this file was built against. */
    if (*(volatile u64 *)(tv + TRAMP_MAGIC_OFF) != TRAMP_MAGIC)
        panic("AP trampoline layout mismatch: data block moved, see "
              "TRAMP_*_OFF in lapic.c and the .org in ap_trampoline.S");

    /* Fill in data */
    *(volatile u64 *)(tv + TRAMP_PML4_OFF)  = pml4_phys;
    *(volatile u64 *)(tv + TRAMP_STACK_OFF) = stack_virt;
    *(volatile u64 *)(tv + TRAMP_PCPU_OFF)  = percpu_ptr;
    *(volatile u64 *)(tv + TRAMP_CPU_OFF)   = cpu_id;
    *(volatile u64 *)(tv + TRAMP_READY_OFF) = 0;   /* ready flag init */
    *(volatile u64 *)(tv + TRAMP_APMAIN_OFF) = (u64)(usize)ap_main;
}

/* ── AP-worker trampoline ──
 * Entered (via arch_context_switch from ap_main) on an idle AP after it steals
 * a stealable CPU-bound kernel worker. Runs the worker's entry with interrupts
 * disabled (ap_main disabled them), then parks back to the AP idle context.
 *
 * This deliberately does NOT touch the global current_task or go through the
 * cooperative scheduler_yield/exit path — those are BSP-owned and not SMP-safe.
 * The AP tracks the worker solely via its per-CPU struct. */
void ap_worker_trampoline(void) {
    struct percpu *pcpu = get_percpu();
    struct task *t = pcpu ? pcpu->cur_task : (struct task *)0;

    if (t && t->entry)
        t->entry(t->arg);
    if (t) {
        /* Claim stack_released before publishing DEAD so any reaper (today
         * only sched_ap_reap_worker on the same CPU after switch-back, but
         * keep the protocol symmetric) sees released==0 until the
         * arch_context_switch below swaps RSP off this stack. */
        t->stack_released = 0;
        t->state = TASK_DEAD;  /* tells ap_main the worker has finished */
    }

    /* Switch back to the AP idle loop. Saves our now-defunct context into
     * t->context (never reused) and restores the AP idle context captured in
     * ap_main. Does not return. */
    arch_context_switch(&t->context, (struct cpu_context *)pcpu->sched_return_ctx,
                        &t->stack_released);

    for (;;) __asm__ volatile("hlt");  /* unreachable */
}

/* ── AP entry point ──
 * Called by the trampoline after it enters 64-bit long mode.
 * cpu_id is a CPU index (0=BSP, 1+ = AP).
 * This function runs on the AP with its own stack and per-CPU data. */
void ap_main(u32 cpu_id) {
    struct percpu *pcpu = get_percpu();

    /* Mark online FIRST, before anything that could stall, so the BSP's
     * get_online_cpu_count() observes this AP promptly. We deliberately do NOT
     * call console_write from an AP during the work-stealing phase: the
     * framebuffer console's cursor globals and the shared serial port are
     * unsynchronised there. (Once the BKL is held — the userspace phase below —
     * console writes are serialised and safe.) */
    if (pcpu) {
        pcpu->cpu_online = 1;
        pcpu->scheduler_started = 1;
    }

    /* Per-CPU arch init so this AP can run ring 3: kernel GDT/IDT, this CPU's
     * TSS, and the SYSCALL/SSE MSRs. Harmless for the work-stealing path (which
     * stays in ring 0), required before the cooperative phase touches userspace. */
    x86_ap_arch_init((int)cpu_id);

    /* The AP idle context. ap_main never returns, so this local persists for
     * the AP's lifetime; a stolen worker switches back into it when it parks. */
    struct cpu_context idle_ctx;

    /* ── Phase 1: work-stealing only ──
     * Until the BSP finishes the SMP self-test (g_ap_userspace_enabled), run
     * only stealable CPU-bound kernel workers, with interrupts disabled and
     * WITHOUT the BKL (workers are self-contained and SMP-safe via their own
     * test lock). This preserves the M24b work-stealing path exactly. */
    while (!g_ap_userspace_enabled) {
        interrupts_disable();

        struct task *t = pcpu ? rq_dequeue(&pcpu->runqueue) : (struct task *)0;
        /* Claim it the way the other pickers do.
         *
         * A plain "state == READY" read is not a claim: the state says nothing
         * about whether a CPU is still executing the task (a waker can mark a
         * live task READY), and the stack lease stays published for as long as
         * it runs. Taking a task that another CPU is on puts two CPUs on one
         * kernel stack, which surfaces later as a return into a small integer.
         */
        if (t && !(t->state == TASK_READY && t->stealable &&
                   __atomic_load_n(&t->stack_released, __ATOMIC_ACQUIRE) &&
                   !task_running_somewhere(t))) {
            rq_enqueue(&pcpu->runqueue, t);
            t = NULL;
        }
        if (t) {
            enum task_state expected = TASK_READY;

            if (!__atomic_compare_exchange_n(&t->state, &expected, TASK_RUNNING,
                                             0, __ATOMIC_ACQUIRE,
                                             __ATOMIC_RELAXED)) {
                rq_enqueue(&pcpu->runqueue, t);
                t = NULL;
            } else {
                __atomic_store_n(&t->stack_released, 0, __ATOMIC_RELEASE);
            }
        }
        if (!t)
            t = sched_steal_task();  /* only returns READY stealable workers */

        if (t) {
            t->state = TASK_RUNNING;
            pcpu->cur_task = t;
            pcpu->sched_return_ctx = &idle_ctx;
            /* OLD here is the AP idle context (stack-local cpu_context, no
             * task struct, never reaped) — pass NULL to skip the publish. */
            arch_context_switch(&idle_ctx, &t->context, (volatile int *)0);
            pcpu->cur_task = NULL;
            sched_ap_reap_worker(t);
            interrupts_enable();
            continue;
        }

        interrupts_enable();
        for (volatile int i = 0; i < 100000; i++)
            __asm__ volatile("pause");
    }

    /* ── Phase 2: full cooperative scheduler ──
     * Run ordinary userspace processes (and kernel threads) from the global
     * runqueue under the Big Kernel Lock. The AP runs its dedicated idle task as
     * current_task; scheduler_yield switches into a runnable task (releasing the
     * BKL when that task enters ring 3, so userspace runs in parallel with other
     * cores) and parks back to the idle task when nothing is left to run. */

    /* M28-A: arm THIS AP's LAPIC timer at the same rate the BSP uses, so the
     * per-CPU scheduler tick fires here too — the period is read back rather
     * than repeated, or the cores would count time differently.
     * Interrupts are already enabled here (Phase 1 ended with
     * interrupts_enable()) and the depth-1 BKL acquired below recursively
     * re-enters from the timer ISR — both already wired by M24b. */
    lapic_timer_start_periodic_ms(1000u / sched_tick_hz());

    struct task *idle = scheduler_setup_ap_idle((int)cpu_id, pcpu->kernel_stack_virt);
    pcpu->idle_task = idle;
    pcpu->cur_task = idle;       /* current_task = this AP's idle task */
    pcpu->sched_return_ctx = 0;

    /* AP idle loop. BKL fully retired (M28 #7): no lock to hold across the
     * context switch, so just yield and park when this AP has no work. */
    for (;;) {
        int switched = scheduler_yield();
        if (!switched) {
            __asm__ volatile("sti; hlt" : : : "memory");
        }
    }
}

/* LAPIC-timer frequency, calibrated at lapic_init via the PIT (A2 audit item).
 * Replaces the previous busy-loop "calibration" that was tied to the host
 * CPU's pause-loop speed rather than wall-clock time. Stored as ticks per
 * millisecond so callers (AP per-CPU ticks once we switch the scheduler off
 * the PIT) can derive any cadence. 0 means uncalibrated. */
static u32 g_lapic_ticks_per_ms = 0;

u32 lapic_ticks_per_ms(void) {
    return g_lapic_ticks_per_ms;
}

/* PIT channel 2 (the speaker timer) — runs at 1193182 Hz, doesn't raise IRQ0,
 * and is independent of channel 0 (which the scheduler ticks at TIMER_HZ).
 * Bit 0 of port 0x61 gates the counter; bit 5 mirrors PIT2 OUT (high when the
 * count reaches zero in mode 0). */
#define PIT2_CHANNEL  0x42
#define PIT2_COMMAND  0x43
#define PIT2_GATE     0x61
#define PIT_HZ        1193182U

extern u8 inb(u16 port);
extern void outb(u16 port, u8 value);

/* Timestamp counter. Serialised on both ends of the calibration window so the
 * two reads bracket exactly the interval the PIT measured. */
static inline u64 arch_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void apic_timer_calibrate_against_pit(void) {
    /* 1) Quiesce: disable speaker, disable gate. */
    u8 gate = inb(PIT2_GATE);
    outb(PIT2_GATE, (u8)(gate & ~0x03));

    /* 2) Program PIT2 for mode 0 (interrupt on terminal count), binary,
     *    lobyte+hibyte access, channel 2: 0b10110000 = 0xB0. */
    outb(PIT2_COMMAND, 0xB0);

    /* 3) Pick a count that produces ~10 ms: 11932 / 1193182 Hz ≈ 10 ms.
     *    Avoid 0 (= 65536). */
    const u32 pit_count = 11932;
    outb(PIT2_CHANNEL, (u8)(pit_count & 0xFF));
    outb(PIT2_CHANNEL, (u8)((pit_count >> 8) & 0xFF));

    /* 4) Arm LAPIC timer: one-shot, divide by 16, masked vector, max init. */
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_LVT_ONESHOT);

    /* 5) Open the gate (bit 0) — this starts both counters in lock-step.
     *    Setting INITCNT and the gate back-to-back keeps the window tight. */
    outb(PIT2_GATE, (u8)((gate & ~0x02) | 0x01));
    lapic_write(LAPIC_TIMER_INITCNT, 0xFFFFFFFFU);
    u64 tsc_start = arch_rdtsc();

    /* 6) Wait for PIT2 OUT to go high (count reached zero). */
    while ((inb(PIT2_GATE) & 0x20) == 0)
        __asm__ volatile("pause");

    /* 7) Snapshot LAPIC remaining count and the TSC, then mask the timer. */
    u64 tsc_end = arch_rdtsc();
    u32 end = lapic_read(LAPIC_TIMER_CURCNT);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    /* Restore PIT2 gate to whatever it was (speaker off either way). */
    outb(PIT2_GATE, (u8)(gate & ~0x03));

    /* 8) LAPIC ticks consumed during ~10 ms, scaled to per-ms.
     *    (max - end) / 10 — divide-by-16 already applied at LVT level. */
    u32 elapsed = 0xFFFFFFFFU - end;
    g_lapic_ticks_per_ms = elapsed / 10U;

    /* The same 10 ms window measures the CPU's own clock: the TSC advances at
     * a fixed rate on every CPU b1nix runs on (constant_tsc), so ticks/10 ms
     * scaled to kHz IS the processor frequency. This is what /proc/cpuinfo's
     * "cpu MHz" and /sys/.../cpufreq report; without it the only honest answer
     * was to publish nothing at all. */
    {
        u64 tsc_elapsed = (tsc_end > tsc_start) ? (tsc_end - tsc_start) : 0;
        u32 exact_khz = arch_tsc_khz_from_cpuid();

        /* Prefer the figure the CPU states outright.
         *
         * CPUID leaf 15h publishes the TSC-to-core-crystal ratio and the
         * crystal's frequency, which is the TSC rate exactly — no measurement,
         * no window, no error. The PIT loop above can only ever estimate it,
         * and whatever it gets wrong is carried straight into
         * arch_tsc_monotonic_ns, i.e. into every clock_gettime the system
         * answers. An error of a few hundred parts per million is invisible in
         * a boot message and obvious over an hour of uptime.
         *
         * The measurement stays as the fallback: leaf 15h is absent on older
         * parts and reports a zero crystal frequency on some hypervisors. */
        if (exact_khz)
            arch_set_cpu_khz(exact_khz);
        else if (tsc_elapsed)
            arch_set_cpu_khz(tsc_elapsed / 10U); /* ticks per 10 ms -> kHz */
    }

    /* The frequency is known now, so the cycle counter can serve as a clock —
     * it is what gives clock_gettime a resolution better than one tick. */
    arch_tsc_clock_init();

    console_write("lapic: calibrated against PIT: ");
    console_write_dec(g_lapic_ticks_per_ms);
    console_write(" ticks/ms (div=16), monotonic clock: ");
    console_write(arch_tsc_clock_ready() ? "TSC\n" : "tick (no invariant TSC)\n");
}

/* AP kernel stacks: size, and the paint that makes their use measurable.
 *
 * Same size as the boot CPU's stack, for the same reason -- the deepest paths
 * in this kernel are the same on any CPU. The paint is a byte the stack is
 * filled with before the AP starts; how much of it survives is how much of the
 * stack was never touched, which is the only honest way to answer "is this big
 * enough" and the thing whose absence let 16 KiB stand. */
#define AP_KSTACK_SIZE  262144u
#define AP_KSTACK_PAINT 0x5a

static u64 ap_kstack_base[MAX_CPUS];

/* Bytes of the deepest CPU's stack that were ever used, and its total. Returns
 * 0 when no AP has been brought up. */
u64 ap_stack_peak(u64 *total_out) {
  u64 worst = 0;

  if (total_out)
    *total_out = AP_KSTACK_SIZE;
  for (int c = 0; c < (int)MAX_CPUS; c++) {
    const u8 *b = (const u8 *)(usize)ap_kstack_base[c];
    if (!b)
      continue;
    u64 clean = 0;
    while (clean < AP_KSTACK_SIZE && b[clean] == AP_KSTACK_PAINT)
      clean++;
    u64 used = AP_KSTACK_SIZE - clean;
    if (used > worst)
      worst = used;
  }
  return worst;
}

/* INIT must stay asserted for 10 ms (Intel SDM, universal start-up algorithm),
 * and a CPU that is going to answer a SIPI does so in well under a millisecond
 * — 100 ms is a generous ceiling that keeps a machine with a candidate APIC ID
 * that does not exist (the no-ACPI path guesses those) from spending seconds
 * per miss. */
#define SMP_INIT_HOLD_NS  (10ull * 1000 * 1000)
#define SMP_SIPI_WAIT_NS  (100ull * 1000 * 1000)

static void smp_delay_ns(u64 ns) {
    u64 deadline = ktime_monotonic_ns() + ns;

    while (ktime_monotonic_ns() < deadline)
        __asm__ volatile("pause");
}

/* Poll the trampoline's ready flag until it is set or the deadline passes.
 * Returns 1 if the AP reported in. */
static int smp_wait_ready(u64 flag_addr, u64 timeout_ns) {
    u64 deadline = ktime_monotonic_ns() + timeout_ns;

    do {
        if (*(volatile u32 *)(usize)flag_addr)
            return 1;
        __asm__ volatile("pause");
    } while (ktime_monotonic_ns() < deadline);

    return *(volatile u32 *)(usize)flag_addr ? 1 : 0;
}

/* Bring up Application Processors.
 * Returns number of CPUs successfully brought up (including BSP). */
int smp_boot_aps(void) {
    int ap_count = 0;

    /* Use the ACTIVE kernel PML4 (from CR3), not the boot-time `pml4` symbol.
     * The boot `pml4` only maps low identity + the kernel image; paging_init
     * switched to a fresh runtime PML4 that additionally maps the kheap and the
     * full direct map. AP kernel stacks are kmalloc'd from the kheap (high
     * addresses), so an AP running on the boot `pml4` faults on its first stack
     * push (the call into ap_main) — before reaching any C code. */
    u64 cr3_val;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3_val));
    u64 pml4_phys = cr3_val & ~0xFFFULL;  /* mask CR3 flag bits, keep frame addr */
    console_write("smp: kernel PML4 at 0x");
    console_write_hex64(pml4_phys);
    console_write("\n");

    /* Build the AP bring-up list.
     *
     * Preferred source: ACPI MADT (real firmware-discovered APIC IDs, no
     * assumption that they are 0..N-1 contiguous). Fall back to CPUID-derived
     * count + the QEMU-style contiguous-APIC-ID assumption when ACPI is
     * unavailable. The BSP's APIC ID is filtered out: we are it.
     */
    u32 bsp_apic = lapic_id();
    u32 ap_apic_ids[MAX_CPUS];
    u32 ap_to_bring_up = 0;
    /* How many CPUs this machine is believed to have, when that is known
     * independently of the candidate list. The no-ACPI path probes more
     * candidates than there are CPUs (it cannot know which APIC IDs are real),
     * and each candidate that never answers costs two SIPI timeouts — so the
     * bring-up loop stops as soon as this many CPUs are online. */
    u32 expected_cpus = 0;

    if (acpi_ready() && acpi_cpu_count() > 0) {
        int total = acpi_cpu_count();
        for (int i = 0; i < total && ap_to_bring_up < MAX_CPUS - 1; i++) {
            const struct acpi_cpu_entry *c = acpi_cpu(i);
            if (!c || !c->enabled)
                continue;
            if ((u32)c->apic_id == bsp_apic)
                continue;
            ap_apic_ids[ap_to_bring_up++] = (u32)c->apic_id;
        }
        console_write("smp: ACPI MADT lists ");
        console_write_dec(total);
        console_write(" CPUs, ");
        console_write_dec(ap_to_bring_up);
        console_write(" APs to bring up\n");
    } else {
        /* CPUID fallback (legacy path).
         *
         * Leaf 0x0B (extended topology) returns, per subleaf, the level type
         * in ECX[15:8] (0 = invalid/last) and the number of logical processors
         * at that level in EBX[15:0]. The TOTAL logical-processor count is the
         * EBX of the highest valid level. */
        u32 max_leaf;
        __asm__ volatile("cpuid" : "=a"(max_leaf) : "a"(0) : "ebx", "ecx", "edx");
        u32 cpu_count = 1;
        if (max_leaf >= 0x0B) {
            for (u32 sub = 0; sub < 8; sub++) {
                u32 eax, ebx, ecx, edx;
                __asm__ volatile("cpuid"
                                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                 : "a"(0x0B), "c"(sub));
                u32 level_type = (ecx >> 8) & 0xFF;
                if (level_type == 0)
                    break;
                u32 logical = ebx & 0xFFFF;
                if (logical > cpu_count)
                    cpu_count = logical;
            }
        }
        if (cpu_count <= 1) {
            u32 a, b, c, d;
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
            if (d & (1u << 28)) {
                u32 lpc = (b >> 16) & 0xFF;
                if (lpc > cpu_count)
                    cpu_count = lpc;
            }
        }
        if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;
        expected_cpus = cpu_count;
        console_write("smp: CPUID reports ");
        console_write_dec(cpu_count);
        console_write(" logical processors (ACPI absent)\n");

        /* Candidate APIC IDs, spaced the way this CPU's topology actually
         * numbers them rather than assumed to be 0..cpu_count-1.
         *
         * An APIC ID is a packed field: the low `smt_shift` bits select the
         * thread within a core, the next `core_shift - smt_shift` bits the core
         * within a package, and the rest the package. Leaf 0x0B reports each
         * level's shift width in EAX[4:0]. When every thread is enabled the IDs
         * do come out contiguous — which is why the old assumption held on QEMU
         * — but they are sparse the moment a level is partly populated: with
         * SMT disabled in firmware, a 4-core part answers on 0, 2, 4, 6 and the
         * contiguous list asked for 1, 2, 3, missing two real CPUs and probing
         * two that do not exist.
         *
         * Both spacings are tried, closest-first, and the list is bounded: a
         * candidate that is not there simply fails to set the trampoline's
         * ready flag and is skipped by the bring-up loop below, so a wrong
         * guess costs time, never correctness. */
        u32 smt_shift = 0;
        if (max_leaf >= 0x0B) {
            u32 eax, ebx, ecx, edx;
            __asm__ volatile("cpuid"
                             : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(0x0B), "c"(0));
            if (((ecx >> 8) & 0xFF) == 1) /* level type 1 = SMT */
                smt_shift = eax & 0x1F;
        }
        u32 stride = 1u << smt_shift;
        u32 max_id = cpu_count * stride;
        if (max_id > 256) max_id = 256;

        for (u32 id = 0; id < max_id && ap_to_bring_up < MAX_CPUS - 1; id++) {
            if (id == bsp_apic) continue;
            /* Contiguous IDs first (every thread enabled), then the stride-
             * spaced ones a partly-populated SMT level produces. Skipping the
             * duplicates keeps the list to at most cpu_count-1 entries when the
             * two spacings agree, i.e. stride == 1. */
            if (stride > 1 && (id % stride) != 0 && id >= cpu_count)
                continue;
            ap_apic_ids[ap_to_bring_up++] = id;
        }
        console_write("smp: APIC ID stride ");
        console_write_dec(stride);
        console_write(", ");
        console_write_dec(ap_to_bring_up);
        console_write(" candidates to probe\n");
    }

    /* If only BSP, nothing to do */
    if (ap_to_bring_up == 0) {
        console_write("smp: single CPU mode\n");
        return 1;
    }

    /* Bring up APs one by one. cpu_id is a contiguous index assigned in the
     * order APs successfully come online; apic_id is the firmware-discovered
     * physical APIC ID (the value the LAPIC ICR/MMIO actually wants). */
    for (u32 i = 0; i < ap_to_bring_up; i++) {
        if (expected_cpus && (u32)(ap_count + 1) >= expected_cpus)
            break; /* every CPU this machine has is up; the rest are guesses */
        u32 apic_id = ap_apic_ids[i];
        int cpu_id = (int)(i + 1);
        console_write("smp: bringing up AP apic_id=");
        console_write_dec(apic_id);
        console_write(" cpu_id=");
        console_write_dec(cpu_id);
        console_write("\n");

        /* Allocate per-CPU data (4KB-aligned) */
        u8 *pcpu_raw = kzalloc(sizeof(struct percpu) + 4096);
        if (!pcpu_raw) { console_write("smp: percpu alloc failed\n"); continue; }
        struct percpu *pcpu = (struct percpu *)(((u64)(usize)pcpu_raw + 4095) & ~(u64)4095);
        memset(pcpu, 0, sizeof(struct percpu));
        pcpu->cpu_id = cpu_id;
        pcpu->apic_id = apic_id;

        /* Allocate kernel stack.
         *
         * 16 KiB was far too small and the third instance of the same defect:
         * the boot CPU's stack needed 256 KiB (it peaked at 103 KiB on the
         * widest device tree), and the syscall path alone carries a
         * 28,584-byte frame -- nearly twice this whole stack. An AP that
         * overflowed ran off the end of a kmalloc block into whatever the heap
         * had next, silently, which is what corruption of an unrelated
         * process's state looks like from userspace. APs exist only on SMP,
         * which is why the symptom only ever appeared there.
         *
         * Painted so the depth actually reached can be measured rather than
         * assumed; ap_stack_peak() reports it. */
        void *stack = kmalloc(AP_KSTACK_SIZE);
        if (!stack) { kfree(pcpu_raw); console_write("smp: stack alloc failed\n"); continue; }
        memset(stack, AP_KSTACK_PAINT, AP_KSTACK_SIZE);
        ap_kstack_base[cpu_id] = (u64)(usize)stack;
        u64 stack_top = ((u64)(usize)stack + AP_KSTACK_SIZE) & ~0xFULL;
        pcpu->kernel_stack_virt = stack_top;

        /* Index the per-CPU table by the contiguous cpu_id, not the
         * (potentially sparse) APIC ID — get_percpu_n(idx) walks idx
         * 0..MAX_CPUS-1 as a CPU number. */
        ap_cpu_data[cpu_id] = pcpu;

        /* Setup trampoline at 0x8000 */
        smp_setup_trampoline(pml4_phys, stack_top, (u64)(usize)pcpu, cpu_id);

        /* Step 1: Send INIT IPI, then hold it asserted for the 10 ms the
         * Intel MP spec's universal start-up algorithm requires.
         *
         * The hold and both waits below used to be `for (volatile int i = 0;
         * i < 5000000; i++) pause` — a number with no unit, whose real
         * duration depended on the host's clock and on how the compiler felt
         * about the loop. Same class of defect M118 removed from the drivers.
         * The monotonic clock is calibrated long before smp_boot_aps runs
         * (lapic_init hands it to the TSC at main.c), so these are real
         * durations. */
        console_write("smp: sending INIT...\n");
        lapic_send_ipi(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_TRIGGER_LEVEL);
        smp_delay_ns(SMP_INIT_HOLD_NS);
        lapic_send_ipi(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL_DEASSERT | LAPIC_ICR_TRIGGER_LEVEL);

        /* Step 2: Send SIPI. The start-up vector is the trampoline page number:
         * the AP begins executing at (vector << 12). The trampoline lives at
         * physical 0x8000, so the vector is 0x08 (0x8000 >> 12), NOT 0x80 —
         * vector 0x80 would start the AP at 0x80000, where there is no code. */
        console_write("smp: sending SIPI...\n");
        lapic_send_ipi(apic_id, LAPIC_ICR_STARTUP | 0x08);

        /* Wait for ready flag */
        u64 tv = 0xFFFF800000000000ULL + 0x8000;
        if (smp_wait_ready(tv + TRAMP_READY_OFF, SMP_SIPI_WAIT_NS)) {
            console_write("smp: AP ");
            console_write_dec(apic_id);
            console_write(" ready!\n");
            ap_count++;
            goto ap_done;
        }

        /* If first SIPI didn't work, try a second */
        console_write("smp: retrying SIPI...\n");
        lapic_send_ipi(apic_id, LAPIC_ICR_STARTUP | 0x08);
        if (smp_wait_ready(tv + TRAMP_READY_OFF, SMP_SIPI_WAIT_NS)) {
            console_write("smp: AP ");
            console_write_dec(apic_id);
            console_write(" ready!\n");
            ap_count++;
            goto ap_done;
        }

        console_write("smp: AP failed to start\n");
        kfree(pcpu_raw);
        kfree(stack);
        continue;

ap_done:
        ;
    }

    console_write("smp: total CPUs: ");
    console_write_dec(ap_count + 1);
    console_write("\n");
    /* Publish the runtime CPU count for loop-bound consumers (work-stealing,
     * IPI fan-outs, etc.) so they don't always walk to MAX_CPUS. */
    g_max_cpus = ap_count + 1;
    return ap_count + 1;
}

/* ── SMP percpu accessors (used by task stealing) ── */

/* Returns percpu for index idx (0 = BSP, 1..N = APs).
 * Returns NULL if idx is out of range or the CPU is not online. */
struct percpu *get_percpu_n(int idx)
{
    if (idx < 0 || idx >= MAX_CPUS)
        return (struct percpu *)0;
    if (idx == 0) {
        return boot_cpu_data.cpu_online ? &boot_cpu_data : (struct percpu *)0;
    }
    struct percpu *p = ap_cpu_data[idx];
    if (!p || !p->cpu_online)
        return (struct percpu *)0;
    return p;
}

/* Returns total number of online CPUs (BSP + APs). Bounded by the runtime
 * CPU count instead of MAX_CPUS so big-ceiling configs don't pay a 64-slot
 * scan on small machines. */
int get_online_cpu_count(void)
{
    int count = boot_cpu_data.cpu_online ? 1 : 0;
    int upper = g_max_cpus < MAX_CPUS ? g_max_cpus : MAX_CPUS;
    for (int i = 1; i < upper; i++) {
        struct percpu *p = ap_cpu_data[i];
        if (p && p->cpu_online)
            count++;
    }
    return count;
}
