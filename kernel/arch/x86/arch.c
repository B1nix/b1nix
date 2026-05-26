#include <b1nix/arch.h>
#include <b1nix/console.h>
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

static struct x86_tss x86_tss __attribute__((aligned(16)));

void x86_idt_init(void);
void x86_pic_init(void);
void x86_timer_init(void);
void rtc_init(void);

extern void x86_syscall_entry(void);
extern u64 gdt64_tss[2];
extern char x86_syscall_stack_top[];

static void x86_enable_write_protect(void) {
  u64 cr0;
  __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
  cr0 |= (1ULL << 16);
  __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");
}

static void x86_tss_init(void) {
  u64 base = (u64)&x86_tss;
  u32 limit = sizeof(x86_tss) - 1;

  x86_tss.rsp0 = (u64)x86_syscall_stack_top;
  x86_tss.iomap_base = sizeof(x86_tss);

  gdt64_tss[0] = ((u64)(limit & 0xffff)) | ((base & 0xffffff) << 16) |
                 ((u64)0x89 << 40) | ((u64)((limit >> 16) & 0xf) << 48) |
                 ((u64)((base >> 24) & 0xff) << 56);
  gdt64_tss[1] = base >> 32;

  __asm__ volatile("ltr %0" : : "r"((u16)X86_TSS_SELECTOR) : "memory");
}

void arch_set_kernel_stack(u64 stack_top) {
  x86_tss.rsp0 = stack_top;
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

void arch_halt(void) {
  /* QEMU isa-debug-exit: exit with status (val << 1) | 1. */
  __asm__ volatile("outb %0, %1" : : "a"((u8)0), "Nd"((u16)0xf4));

  for (;;) {
    __asm__ volatile("hlt");
  }
}
