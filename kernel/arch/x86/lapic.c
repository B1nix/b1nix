#include <b1nix/arch.h>
#include <b1nix/lapic.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/io.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/runqueue.h>
#include <string.h>

/* External functions */
extern void arch_context_switch(struct cpu_context *old, struct cpu_context *new);
extern void paging_switch_address_space(u64 pml4_phys);
extern void arch_set_kernel_stack(u64 stack);

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

void lapic_send_ipi(u32 apic_id, u32 icr_low) {
    /* Write destination APIC ID to ICR high */
    lapic_write(LAPIC_ICR_HIGH, (u64)apic_id << 32);
    /* Write command to ICR low */
    lapic_write(LAPIC_ICR_LOW, icr_low);
    /* Wait for delivery to complete */
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
        __asm__ volatile("pause");
}

void lapic_send_ipi_allbutself(u32 icr_low) {
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, icr_low | LAPIC_ICR_DEST_OTHERS);
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
        __asm__ volatile("pause");
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

    /* Software enable LAPIC via SVR */
    u32 svr = lapic_read(LAPIC_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr &= ~LAPIC_SVR_FOCUS_DISABLE; /* enable focus checking */
    svr = (svr & ~0xFF) | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    /* Mask unused LVT entries (not LINT0/LINT1 — they carry PIC interrupts) */
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERFMON, LAPIC_LVT_MASKED);

    /* Set task priority to accept all interrupts */
    lapic_write(LAPIC_TPR, 0);

    console_write("lapic: initialized (id=");
    console_write_dec(lapic_id());
    console_write(")\n");
}

/* ── Per-CPU data ── */

static struct percpu boot_cpu_data = {
    .cpu_id = 0,
    .apic_id = 0,
    .current_task = 0,
    .scheduler_ticks = 0,
    .scheduler_started = 0,
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
#include "../../build/x86/ap_trampoline.inc"

/* Extern for kernel PML4 (defined in boot.S) */
extern u64 pml4[];

/* Per-CPU data for APs */
static struct percpu *ap_cpu_data[MAX_CPUS];

/* AP trampoline — flat binary linked at 0x8000.
 * Generated by kernel/arch/x86/ap_trampoline.S. */
extern u8 ap_trampoline_bin[];
extern u32 ap_trampoline_bin_len;

/* Define the data layout offsets within the trampoline page (from nm output) */
#define TRAMP_PML4_OFF    0xB8   /* pml4_phys */
#define TRAMP_STACK_OFF   0xC0   /* stack_ptr */
#define TRAMP_PCPU_OFF    0xC8   /* percpu_ptr */
#define TRAMP_CPU_OFF     0xD0   /* cpu_id */
#define TRAMP_GDT_PTR_OFF 0xD8   /* gdt_ptr (2 limit + 4 base = 6 bytes) */
#define TRAMP_READY_OFF   0xE2   /* ready_flag (4 bytes) */
#define TRAMP_APMAIN_OFF  0xE6   /* ap_main_ptr (8 bytes) */
#define TRAMP_GDT_OFF     0xF0   /* gdt entries */

static void smp_setup_trampoline(u64 pml4_phys, u64 stack_virt,
                                  u64 percpu_ptr, u32 cpu_id)
{
    u64 tv = 0xFFFF800000000000ULL + 0x8000;
    usize tramp_len = (usize)ap_trampoline_bin_len;

    if (tramp_len > 0x1000) tramp_len = 0x1000;

    /* Copy trampoline code */
    for (usize i = 0; i < tramp_len; i++)
        *(volatile u8 *)(tv + i) = ap_trampoline_bin[i];

    /* Fill in data */
    *(volatile u64 *)(tv + TRAMP_PML4_OFF)  = pml4_phys;
    *(volatile u64 *)(tv + TRAMP_STACK_OFF) = stack_virt;
    *(volatile u64 *)(tv + TRAMP_PCPU_OFF)  = percpu_ptr;
    *(volatile u64 *)(tv + TRAMP_CPU_OFF)   = cpu_id;
    *(volatile u64 *)(tv + TRAMP_READY_OFF) = 0;   /* ready flag init */
    *(volatile u64 *)(tv + TRAMP_APMAIN_OFF) = (u64)(usize)ap_main;
}

/* Timer calibration: how many APIC timer ticks per scheduler tick (10ms) */
/* ── AP entry point ──
 * Called by the trampoline after it enters 64-bit long mode.
 * cpu_id is a CPU index (0=BSP, 1+ = AP).
 * This function runs on the AP with its own stack and per-CPU data. */
void ap_main(u32 cpu_id) {
    struct percpu *pcpu = get_percpu();

    console_write("AP: cpu ");
    console_write_dec(cpu_id);
    console_write(" online (apic_id=");
    console_write_dec(pcpu ? pcpu->apic_id : 0);
    console_write(")\n");

    if (pcpu) {
        pcpu->cpu_online = 1;
        pcpu->scheduler_started = 1;
    }

    /* Enable interrupts — AP will receive timer ticks via APIC */
    interrupts_enable();

    /* Idle loop: halt until interrupt, then try to find work */
    for (;;) {
        struct task *t = NULL;

        /* Try our per-CPU runqueue first */
        if (pcpu)
            t = rq_dequeue(&pcpu->runqueue);

        /* If nothing, try to steal from other CPUs */
        if (!t)
            t = sched_steal_task();

        /* If we found a task, run it */
        if (t && t->state == TASK_READY) {
            t->state = TASK_RUNNING;
            pcpu->current_task = t;
            current_task = t;
            paging_switch_address_space(t->pml4_phys);
            arch_set_kernel_stack(t->kernel_stack_ptr);
            arch_context_switch(&t->context, &t->context); /* dummy — real switch */
            /* Back to idle */
            pcpu->current_task = NULL;
            current_task = NULL;
        }

        __asm__ volatile("sti; hlt");
    }
}

static u32 apic_timer_calibrate(void) {
    u32 max_count = 0xFFFFFFFF;
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_ONESHOT);
    lapic_write(LAPIC_TIMER_INITCNT, max_count);
    (void)lapic_read(LAPIC_TIMER_CURCNT); /* reset counter */
    for (volatile int i = 0; i < 10000000; i++)
        __asm__ volatile("pause");
    u32 end = lapic_read(LAPIC_TIMER_CURCNT);
    u32 elapsed = max_count - end;
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    console_write("lapic: timer calibration ~");
    console_write_dec(elapsed);
    console_write(" ticks per 10ms\n");
    return elapsed ? elapsed : 100000;
}

/* Bring up Application Processors.
 * Returns number of CPUs successfully brought up (including BSP). */
int smp_boot_aps(void) {
    int ap_count = 0;

    /* Get PML4 physical address */
    u64 pml4_phys = (u64)(usize)pml4;
    console_write("smp: kernel PML4 at 0x");
    console_write_hex64(pml4_phys);
    console_write("\n");

    /* Determine number of CPUs from CPUID */
    u32 max_leaf;
    __asm__ volatile("cpuid" : "=a"(max_leaf) : "a"(0) : "ebx", "ecx", "edx");
    u32 cpu_count = 1;
    if (max_leaf >= 0x0B) {
        u32 eax, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x0B), "c"(0));
        if (ecx & 0xFF) {
            cpu_count = ebx & 0xFFFF;
            console_write("smp: CPUID reports ");
            console_write_dec(cpu_count);
            console_write(" logical processors\n");
        }
    }
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

    /* If only 1 CPU, nothing to do */
    if (cpu_count <= 1) {
        console_write("smp: single CPU mode\n");
        return 1;
    }

    /* Bring up APs one by one */
    for (u32 apic_id = 1; apic_id < cpu_count; apic_id++) {
        int cpu_id = (int)apic_id;
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

        /* Allocate kernel stack */
        void *stack = kmalloc(16384);
        if (!stack) { kfree(pcpu_raw); console_write("smp: stack alloc failed\n"); continue; }
        u64 stack_top = ((u64)(usize)stack + 16384) & ~0xFULL;
        pcpu->kernel_stack_virt = stack_top;

        ap_cpu_data[apic_id] = pcpu;

        /* Setup trampoline at 0x8000 */
        smp_setup_trampoline(pml4_phys, stack_top, (u64)(usize)pcpu, cpu_id);

        /* Step 1: Send INIT IPI */
        console_write("smp: sending INIT...\n");
        lapic_send_ipi(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_TRIGGER_LEVEL);
        for (volatile int i = 0; i < 5000000; i++) __asm__ volatile("pause");
        lapic_send_ipi(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL_DEASSERT | LAPIC_ICR_TRIGGER_LEVEL);

        /* Step 2: Send SIPI (vector = 0x80 = 0x8000 >> 12) */
        console_write("smp: sending SIPI...\n");
        lapic_send_ipi(apic_id, LAPIC_ICR_STARTUP | 0x80);

        /* Wait for ready flag */
        u64 tv = 0xFFFF800000000000ULL + 0x8000;
        for (volatile int wait = 0; wait < 50000000; wait++) {
            if (*(volatile u32 *)(tv + TRAMP_READY_OFF)) {
                console_write("smp: AP ");
                console_write_dec(apic_id);
                console_write(" ready!\n");
                ap_count++;
                goto ap_done;
            }
            __asm__ volatile("pause");
        }

        /* If first SIPI didn't work, try a second */
        console_write("smp: retrying SIPI...\n");
        lapic_send_ipi(apic_id, LAPIC_ICR_STARTUP | 0x80);
        for (volatile int wait = 0; wait < 50000000; wait++) {
            if (*(volatile u32 *)(tv + TRAMP_READY_OFF)) {
                console_write("smp: AP ");
                console_write_dec(apic_id);
                console_write(" ready!\n");
                ap_count++;
                goto ap_done;
            }
            __asm__ volatile("pause");
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
    return ap_count + 1;
}
