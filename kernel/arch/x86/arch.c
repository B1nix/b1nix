#include <b1nix/arch.h>
#include <b1nix/console.h>
#include <b1nix/lapic.h>
#include <b1nix/types.h>

#define X86_TSS_SELECTOR 0x28

struct x86_tss {
  u32 reserved0;
  u64 rsp0;
  u64 rsp1;
  u64 rsp2;
  u64 reserved1;
  u64 ist1;
  u64 ist2;
  u64 ist3;
  u64 ist4;
  u64 ist5;
  u64 ist6;
  u64 ist7;
  u64 reserved2;
  u16 reserved3;
  u16 iomap_base;
} __attribute__((packed));

/* One TSS per CPU. TSS.rsp0 is the ring-0 stack a CPU switches to on a ring-3
 * interrupt/exception; it is repointed at the running task's kernel stack on
 * every context switch (arch_set_kernel_stack), so each CPU needs its own TSS
 * to run userspace independently. */
static struct x86_tss x86_tss_arr[MAX_CPUS] __attribute__((aligned(16)));

void x86_idt_init(void);
void x86_idt_load(void); /* interrupts.c — load the shared IDT on this CPU */
void x86_pic_init(void);
void x86_timer_init(void);
void rtc_init(void);

extern void x86_syscall_entry(void);
extern u64 gdt64_tss[];      /* MAX_CPUS TSS descriptors (2 quads each) */
extern u8 gdt64_pointer[];   /* 10-byte GDT descriptor (limit:2 + base:8) */
extern char x86_syscall_stack_top[];

static void x86_enable_write_protect(void) {
  u64 cr0;
  __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
  cr0 |= (1ULL << 16);
  __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");
}

/* Build CPU `cpu`'s TSS descriptor in the shared GDT and load it (ltr). The
 * descriptor pair lives at gdt64_tss[cpu*2 .. cpu*2+1] (selector
 * X86_TSS_SELECTOR + cpu*16). */
static void x86_tss_init_cpu(int cpu) {
  struct x86_tss *t = &x86_tss_arr[cpu];
  u64 base = (u64)t;
  u32 limit = sizeof(*t) - 1;

  if (cpu == 0)
    t->rsp0 = (u64)x86_syscall_stack_top; /* boot value; updated per switch */
  t->iomap_base = sizeof(*t);

  gdt64_tss[cpu * 2 + 0] =
      ((u64)(limit & 0xffff)) | ((base & 0xffffff) << 16) | ((u64)0x89 << 40) |
      ((u64)((limit >> 16) & 0xf) << 48) | ((u64)((base >> 24) & 0xff) << 56);
  gdt64_tss[cpu * 2 + 1] = base >> 32;

  __asm__ volatile("ltr %0"
                   :
                   : "r"((u16)(X86_TSS_SELECTOR + cpu * 16))
                   : "memory");
}

static void x86_tss_init(void) { x86_tss_init_cpu(0); }

void arch_set_kernel_stack(u64 stack_top) {
  struct percpu *p = get_percpu();
  int cpu = p ? (int)p->cpu_id : 0;
  x86_tss_arr[cpu].rsp0 = stack_top;
}

void x86_syscall_init(void) {
  u32 lo, hi;
  /* Fix: Enable syscall/sysret by setting SCE bit in EFER MSR */
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
  lo |= 1; // Установка бита SCE (System Call Enable)
  __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080));

  /* STAR: SYSCALL enters 0x08/0x10, SYSRET returns to 0x20/0x18. */
  hi = (0x10u << 16) | 0x08u;
  __asm__ volatile("wrmsr" : : "a"(0), "d"(hi), "c"(0xC0000081));

  u64 entry = (u64)x86_syscall_entry;
  __asm__ volatile("wrmsr"
                   :
                   : "a"((u32)entry), "d"((u32)(entry >> 32)), "c"(0xC0000082));

  __asm__ volatile("wrmsr" : : "a"(0x200), "d"(0), "c"(0xC0000084));
}

static void x86_enable_sse(void) {
  u64 cr0;
  __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1ULL << 2); // Clear EM (Coprocessor Emulation)
  cr0 |= (1ULL << 1);  // Set MP (Monitor Coprocessor)
  cr0 |= (1ULL << 5);  // Set NE (Numeric Error)
  __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");

  u64 cr4;
  __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ULL << 9);  // Set OSFXSR (FXSAVE/FXRSTOR Support)
  cr4 |= (1ULL << 10); // Set OSXMMEXCPT (SIMD Exception Support)
  __asm__ volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");
}

void arch_init(void) {
  x86_tss_init();
  x86_idt_init();
  x86_pic_init();
  x86_timer_init();
  rtc_init();
  x86_syscall_init();
  x86_enable_write_protect();
  x86_enable_sse();
  __asm__ volatile("sti");
  console_write("arch: x86_64 initialized (syscalls enabled)\n");
}

/* Per-CPU arch init for an Application Processor, run once from ap_main before
 * the AP may execute ring 3. The AP arrives on the trampoline's minimal GDT
 * (no user segments, no TSS) with no IDT and with the SYSCALL/SSE MSRs unset,
 * so it must replicate the BSP's arch_init for itself. */
void x86_ap_arch_init(int cpu) {
  /* Switch to the kernel GDT (it has the user code/data segments and every
   * CPU's TSS descriptor). Reload the data segments and CS, but NEVER %gs:
   * reloading a GS selector in long mode resets the GS base, which holds this
   * CPU's per-CPU pointer (the trampoline set it via wrmsr; b1nix uses no
   * SWAPGS). CS is reloaded with a far return to the kernel code selector. */
  __asm__ volatile("lgdt (%0)" : : "r"(gdt64_pointer) : "memory");
  __asm__ volatile("movw $0x10, %%ax\n\t"
                   "movw %%ax, %%ds\n\t"
                   "movw %%ax, %%es\n\t"
                   "movw %%ax, %%ss\n\t"
                   "movw %%ax, %%fs\n\t"
                   "pushq $0x08\n\t"          /* CS */
                   "leaq 1f(%%rip), %%rax\n\t"
                   "pushq %%rax\n\t"          /* RIP */
                   "lretq\n\t"
                   "1:\n\t"
                   :
                   :
                   : "rax", "memory");

  x86_idt_load();         /* shared kernel IDT — page faults/exceptions on the AP */
  x86_tss_init_cpu(cpu);  /* this CPU's TSS + ltr (ring-3 interrupts need rsp0) */
  x86_syscall_init();     /* per-CPU SYSCALL MSRs: EFER.SCE, STAR, LSTAR, FMASK */
  x86_enable_sse();       /* per-CPU CR0/CR4 for fxsave/fxrstor in ctx switch */
  x86_enable_write_protect();
}

void arch_halt(void) {
  /* QEMU isa-debug-exit: exit with status (val << 1) | 1. */
  __asm__ volatile("outb %0, %1" : : "a"((u8)0), "Nd"((u16)0xf4));

  for (;;) {
    __asm__ volatile("hlt");
  }
}
