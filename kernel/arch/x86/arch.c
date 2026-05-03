#include <b1nix/arch.h>
#include <b1nix/console.h>

void x86_idt_init(void);
void x86_pic_init(void);
void x86_timer_init(void);

void arch_init(void)
{
	x86_idt_init();
	x86_pic_init();
	x86_timer_init();
	__asm__ volatile("sti");
	console_write("arch: x86_64 initialized\n");
}

void arch_halt(void)
{
	for (;;) {
		__asm__ volatile("hlt");
	}
}
