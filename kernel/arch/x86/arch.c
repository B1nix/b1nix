#include <b1nix/arch.h>
#include <b1nix/console.h>

void x86_idt_init(void);
void x86_pic_init(void);
void x86_timer_init(void);

extern void x86_syscall_entry(void);

void x86_syscall_init(void)
{
	u32 lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
	lo |= 1;
	__asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080));

	/* STAR: SYSCALL enters 0x08/0x10, SYSRET returns to 0x20/0x18. */
	hi = (0x10u << 16) | 0x08u;
	__asm__ volatile("wrmsr" : : "a"(0), "d"(hi), "c"(0xC0000081));

	u64 entry = (u64)x86_syscall_entry;
	__asm__ volatile("wrmsr" : : "a"((u32)entry), "d"((u32)(entry >> 32)), "c"(0xC0000082));

	__asm__ volatile("wrmsr" : : "a"(0x200), "d"(0), "c"(0xC0000084));
}

void arch_init(void)
{
	x86_idt_init();
	x86_pic_init();
	x86_timer_init();
	x86_syscall_init();
	__asm__ volatile("sti");
	console_write("arch: x86_64 initialized (syscalls enabled)\n");
}

void arch_halt(void)
{
	for (;;) {
		__asm__ volatile("hlt");
	}
}
