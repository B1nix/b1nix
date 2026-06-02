#include <b1nix/arch.h>
#include <b1nix/lapic.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/io.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>
#include <b1nix/runqueue.h>
#include <b1nix/acpi.h>
#include <string.h>

volatile int g_ap_userspace_enabled = 0;
int g_max_cpus = 1;

extern void arch_context_switch(struct cpu_context *old, struct cpu_context *new,
                                volatile int *released_publish);
extern void paging_switch_address_space(u64 pml4_phys);
extern void arch_set_kernel_stack(u64 stack);

static void apic_timer_calibrate_against_pit(void);

#define LAPIC_VIRT_BASE  ((volatile u32 *)0xFEFFF000UL)
static volatile u32 *lapic_base = 0;

static void lapic_map_base(u32 phys_base) {
    u32 virt = (u32)LAPIC_VIRT_BASE;
    vmm_map_page(virt, phys_base, VMM_PRESENT | VMM_WRITABLE);
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
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_1);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INITCNT, init_count);
}

static volatile int g_lapic_timer_periodic_active = 0;

int lapic_timer_start_periodic_ms(u32 ms) {
    u32 tpms = lapic_ticks_per_ms();
    if (tpms == 0 || ms == 0) return 0;

    u64 init64 = (u64)tpms * (u64)ms;
    if (init64 == 0 || init64 > 0xFFFFFFFFULL) return 0;

    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INITCNT, (u32)init64);

    g_lapic_timer_periodic_active = 1;
    return 1;
}

int lapic_timer_periodic_active(void) { return g_lapic_timer_periodic_active; }

void lapic_send_ipi(u32 apic_id, u32 icr_low) {
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, icr_low);
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
        __asm__ volatile("pause");
}

void lapic_send_ipi_allbutself(u32 icr_low) {
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, icr_low | LAPIC_ICR_DEST_OTHERS);
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
        __asm__ volatile("pause");
}

void lapic_init_local(void) {
    u32 svr = lapic_read(LAPIC_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr &= ~LAPIC_SVR_FOCUS_DISABLE;
    svr = (svr & ~0xFF) | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERFMON, LAPIC_LVT_MASKED);

    lapic_write(LAPIC_TPR, 0);
}

void lapic_init(void) {
    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (!(edx & (1 << 9))) {
        console_write("lapic: APIC not supported by CPU\n");
        return;
    }

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

    u32 phys_base = (u32)apic_base_phys & ~0xFFFUL;
    console_write("lapic: base at 0x");
    console_write_hex32(phys_base);
    console_write("\n");

    lapic_map_base(phys_base);

    u32 ver = lapic_read(LAPIC_VER);
    u32 max_lvt = (ver >> 16) & 0xFF;
    console_write("lapic: version=0x");
    console_write_hex32(ver);
    console_write(" max_lvt=");
    console_write_dec(max_lvt);
    console_write("\n");

    lapic_init_local();

    console_write("lapic: initialized (id=");
    console_write_dec(lapic_id());
    console_write(")\n");

    apic_timer_calibrate_against_pit();
}

struct percpu boot_cpu_data = {
    .cpu_id = 0,
    .apic_id = 0,
    .cur_task = 0,
    .scheduler_ticks = 0,
    .scheduler_started = 0,
};

void arch_set_fs_base_percpu(u32 base);

void percpu_init(void) {
    boot_cpu_data.apic_id = 0;
    boot_cpu_data.cpu_online = 1;
    boot_cpu_data.self_ptr = (u32)&boot_cpu_data;
    arch_set_fs_base_percpu((u32)&boot_cpu_data);

    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (edx & (1 << 9)) {
        u32 apic_id = (ebx >> 24) & 0xFF;
        boot_cpu_data.apic_id = apic_id;
        boot_cpu_data.cpu_id = 0;
    }

    console_write("percpu: BSP initialized (cpu=0 apic_id=");
    console_write_dec(boot_cpu_data.apic_id);
    console_write(")\n");
}

#include "ap_trampoline.inc"

struct percpu *ap_cpu_data[MAX_CPUS];

extern u8 ap_trampoline_bin[];
extern u32 ap_trampoline_bin_len;

static void smp_setup_trampoline(u32 pd_phys, u32 stack_virt, u32 percpu_ptr, u32 gdt_virtual, u32 cpu_id) {
    u32 tv = 0x8000 + DIRECT_MAP_BASE;
    usize tramp_len = (usize)ap_trampoline_bin_len;
    if (tramp_len > 0x1000) tramp_len = 0x1000;

    for (usize i = 0; i < tramp_len; i++)
        *(volatile u8 *)(tv + i) = ap_trampoline_bin[i];

    *(volatile u32 *)(tv + 0x100) = pd_phys;
    *(volatile u32 *)(tv + 0x104) = stack_virt;
    *(volatile u32 *)(tv + 0x108) = percpu_ptr;
    *(volatile u32 *)(tv + 0x10C) = cpu_id;
    *(volatile u32 *)(tv + 0x110) = gdt_virtual;
    *(volatile u32 *)(tv + 0x114) = 0; /* ready flag */
    *(volatile u32 *)(tv + 0x118) = (u32)(usize)ap_main;
}

void ap_worker_trampoline(void) {
    struct percpu *pcpu = get_percpu();
    struct task *t = pcpu ? pcpu->cur_task : (struct task *)0;

    if (t && t->entry)
        t->entry(t->arg);
    if (t) {
        t->stack_released = 0;
        t->state = TASK_DEAD;
    }

    arch_context_switch(&t->context, (struct cpu_context *)pcpu->sched_return_ctx,
                        &t->stack_released);

    for (;;) __asm__ volatile("hlt");
}

void ap_main(u32 cpu_id) {
    struct percpu *pcpu = get_percpu();

    if (pcpu) {
        pcpu->cpu_online = 1;
        pcpu->scheduler_started = 1;
    }

    extern void x86_ap_arch_init(int cpu);
    x86_ap_arch_init((int)cpu_id);

    struct cpu_context idle_ctx;

    while (!g_ap_userspace_enabled) {
        interrupts_disable();

        struct task *t = pcpu ? rq_dequeue(&pcpu->runqueue) : (struct task *)0;
        if (t && !(t->state == TASK_READY && t->stealable)) {
            rq_enqueue(&pcpu->runqueue, t);
            t = NULL;
        }
        if (!t)
            t = sched_steal_task();

        if (t) {
            t->state = TASK_RUNNING;
            pcpu->cur_task = t;
            pcpu->sched_return_ctx = &idle_ctx;
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

    lapic_timer_start_periodic_ms(10);

    struct task *idle = scheduler_setup_ap_idle((int)cpu_id, pcpu->kernel_stack_virt);
    pcpu->idle_task = idle;
    pcpu->cur_task = idle;
    pcpu->sched_return_ctx = 0;

    for (;;) {
        int switched = scheduler_yield();
        if (!switched) {
            __asm__ volatile("sti; hlt" : : : "memory");
        }
    }
}

static u32 g_lapic_ticks_per_ms = 0;

u32 lapic_ticks_per_ms(void) {
    return g_lapic_ticks_per_ms;
}

#define PIT2_CHANNEL  0x42
#define PIT2_COMMAND  0x43
#define PIT2_GATE     0x61
#define PIT_HZ        1193182U

static void apic_timer_calibrate_against_pit(void) {
    u8 gate = inb(PIT2_GATE);
    outb(PIT2_GATE, (u8)(gate & ~0x03));

    outb(PIT2_COMMAND, 0xB0);

    const u32 pit_count = 11932;
    outb(PIT2_CHANNEL, (u8)(pit_count & 0xFF));
    outb(PIT2_CHANNEL, (u8)((pit_count >> 8) & 0xFF));

    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_LVT_ONESHOT);

    outb(PIT2_GATE, (u8)((gate & ~0x02) | 0x01));
    lapic_write(LAPIC_TIMER_INITCNT, 0xFFFFFFFFU);

    while ((inb(PIT2_GATE) & 0x20) == 0)
        __asm__ volatile("pause");

    u32 end = lapic_read(LAPIC_TIMER_CURCNT);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    outb(PIT2_GATE, (u8)(gate & ~0x03));

    u32 elapsed = 0xFFFFFFFFU - end;
    g_lapic_ticks_per_ms = elapsed / 10U;

    console_write("lapic: calibrated against PIT: ");
    console_write_dec(g_lapic_ticks_per_ms);
    console_write(" ticks/ms (div=16)\n");
}

struct gdt_ptr {
  u16 limit;
  u32 base;
} __attribute__((packed));

extern struct gdt_ptr g_cpu_gdt_ptrs[MAX_CPUS];

int smp_boot_aps(void) {
    int ap_count = 0;

    u32 cr3_val;
    __asm__ volatile("movl %%cr3, %0" : "=r"(cr3_val));
    u32 pd_phys = cr3_val & ~0xFFFUL;
    console_write("smp: kernel PD at 0x");
    console_write_hex32(pd_phys);
    console_write("\n");

    u32 bsp_apic = lapic_id();
    u32 ap_apic_ids[MAX_CPUS];
    u32 ap_to_bring_up = 0;

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
        console_write("smp: CPUID reports ");
        console_write_dec(cpu_count);
        console_write(" logical processors (ACPI absent)\n");

        for (u32 id = 0; id < cpu_count && ap_to_bring_up < MAX_CPUS - 1; id++) {
            if (id == bsp_apic) continue;
            ap_apic_ids[ap_to_bring_up++] = id;
        }
    }

    if (ap_to_bring_up == 0) {
        console_write("smp: single CPU mode\n");
        return 1;
    }

    for (u32 i = 0; i < ap_to_bring_up; i++) {
        u32 apic_id = ap_apic_ids[i];
        int cpu_id = (int)(i + 1);
        console_write("smp: bringing up AP apic_id=");
        console_write_dec(apic_id);
        console_write(" cpu_id=");
        console_write_dec(cpu_id);
        console_write("\n");

        u8 *pcpu_raw = kzalloc(sizeof(struct percpu) + 4096);
        if (!pcpu_raw) { console_write("smp: percpu alloc failed\n"); continue; }
        struct percpu *pcpu = (struct percpu *)(((u32)(usize)pcpu_raw + 4095) & ~4095);
        memset(pcpu, 0, sizeof(struct percpu));
        pcpu->cpu_id = cpu_id;
        pcpu->apic_id = apic_id;

        void *stack = kmalloc(16384);
        if (!stack) { kfree(pcpu_raw); console_write("smp: stack alloc failed\n"); continue; }
        u32 stack_top = ((u32)(usize)stack + 16384) & ~0xFULL;
        pcpu->kernel_stack_virt = stack_top;

        ap_cpu_data[cpu_id] = pcpu;

        /* We need the AP to load a copy of the virtual GDT. Pass pointer to g_cpu_gdt_ptrs[cpu_id] */
        u32 gdt_descriptor = (u32)&g_cpu_gdt_ptrs[cpu_id];

        smp_setup_trampoline(pd_phys, stack_top, (u32)(usize)pcpu, gdt_descriptor, cpu_id);

        console_write("smp: sending INIT...\n");
        lapic_send_ipi(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL_ASSERT | LAPIC_ICR_TRIGGER_LEVEL);
        for (volatile int j = 0; j < 5000000; j++) __asm__ volatile("pause");
        lapic_send_ipi(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_LEVEL_DEASSERT | LAPIC_ICR_TRIGGER_LEVEL);

        console_write("smp: sending SIPI...\n");
        lapic_send_ipi(apic_id, LAPIC_ICR_STARTUP | 0x08);

        u32 tv = 0x8000 + DIRECT_MAP_BASE;
        for (volatile int wait = 0; wait < 50000000; wait++) {
            if (*(volatile u32 *)(tv + 0x114)) { /* ready_flag is at offset 0x114 */
                console_write("smp: AP ");
                console_write_dec(apic_id);
                console_write(" ready!\n");
                ap_count++;
                goto ap_done;
            }
            __asm__ volatile("pause");
        }

        console_write("smp: retrying SIPI...\n");
        lapic_send_ipi(apic_id, LAPIC_ICR_STARTUP | 0x08);
        for (volatile int wait = 0; wait < 50000000; wait++) {
            if (*(volatile u32 *)(tv + 0x114)) {
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
    g_max_cpus = ap_count + 1;
    return ap_count + 1;
}

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
