#include <b1nix/arch.h>
#include <b1nix/console.h>

extern void interrupts_init(void);

void arch_init(void)
{
	console_write("aarch64: arch_init\n");
	interrupts_init();
}

void arch_halt(void)
{
	console_write("aarch64: arch_halt\n");
	while (1) {
		__asm__ volatile("wfi");
	}
}
