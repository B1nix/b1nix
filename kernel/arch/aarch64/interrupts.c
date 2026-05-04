#include <b1nix/types.h>
#include <b1nix/console.h>
#include <b1nix/panic.h>
#include <b1nix/sched.h>

#define GICD_BASE 0x08000000ULL
#define GICC_BASE 0x08010000ULL

#define GICD_CTLR        (*(volatile u32 *)(GICD_BASE + 0x000))
#define GICD_TYPER       (*(volatile u32 *)(GICD_BASE + 0x004))
#define GICD_IGROUPR(n)  (*(volatile u32 *)(GICD_BASE + 0x080 + (n) * 4))
#define GICD_ISENABLER(n) (*(volatile u32 *)(GICD_BASE + 0x100 + (n) * 4))
#define GICD_ICENABLER(n) (*(volatile u32 *)(GICD_BASE + 0x180 + (n) * 4))
#define GICD_IPRIORITYR(n) (*(volatile u32 *)(GICD_BASE + 0x400 + (n) * 4))
#define GICD_ITARGETSR(n) (*(volatile u32 *)(GICD_BASE + 0x800 + (n) * 4))
#define GICD_ICFGR(n)    (*(volatile u32 *)(GICD_BASE + 0xc00 + (n) * 4))

#define GICC_CTLR        (*(volatile u32 *)(GICC_BASE + 0x000))
#define GICC_PMR         (*(volatile u32 *)(GICC_BASE + 0x004))
#define GICC_IAR         (*(volatile u32 *)(GICC_BASE + 0x00c))
#define GICC_EOIR        (*(volatile u32 *)(GICC_BASE + 0x010))

#define TIMER_IRQ 30

extern void vector_table_el1(void);

static void gic_init(void)
{
	// Disable distributor
	GICD_CTLR = 0;

	u32 typer = GICD_TYPER;
	u32 lines = (typer & 0x1f) + 1;

	// Disable and clear all interrupts
	for (u32 i = 0; i < lines; i++) {
		GICD_ICENABLER(i) = 0xffffffff;
	}

	// Route all interrupts to CPU 0
	for (u32 i = 8; i < lines * 8; i++) {
		GICD_ITARGETSR(i) = 0x01010101;
	}

	// Set priority to a safe value
	for (u32 i = 0; i < lines * 8; i++) {
		GICD_IPRIORITYR(i) = 0xa0a0a0a0;
	}

	// Enable distributor
	GICD_CTLR = 1;

	// Initialize CPU interface
	GICC_PMR = 0xf0; // Allow all priorities
	GICC_CTLR = 1;   // Enable signaling
}

static void timer_init(void)
{
	// Enable timer interrupt in GIC
	GICD_ISENABLER(TIMER_IRQ / 32) = 1 << (TIMER_IRQ % 32);

	// Get timer frequency
	u64 freq;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));

	// Set timer interval (e.g. 100Hz -> 10ms)
	u64 interval = freq / 100;
	__asm__ volatile("msr cntp_tval_el0, %0" : : "r"(interval));

	// Enable timer and unmask interrupt
	__asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1ULL));
}

void interrupts_init(void)
{
	__asm__ volatile("msr vbar_el1, %0" : : "r"((u64)vector_table_el1));
	
	gic_init();
	timer_init();

	// Enable interrupts at CPU level
	__asm__ volatile("msr daifclr, #2");
	console_write("aarch64: interrupts enabled\n");
}

void aarch64_irq_handler(void)
{
	u32 iar = GICC_IAR;
	u32 irq = iar & 0x3ff;

	if (irq == 1023) {
		return; // Spurious
	}

	if (irq == TIMER_IRQ) {
		// Reset timer
		u64 freq;
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
		u64 interval = freq / 100;
		__asm__ volatile("msr cntp_tval_el0, %0" : : "r"(interval));
		
		scheduler_on_timer_tick();
	} else {
		console_write("Unhandled IRQ: ");
		console_write_dec(irq);
		console_write("\n");
	}

	GICC_EOIR = iar;
}

void aarch64_sync_handler(u64 esr, u64 elr, u64 far)
{
	console_write("\nException!\nESR: ");
	console_write_hex64(esr);
	console_write("\nELR: ");
	console_write_hex64(elr);
	console_write("\nFAR: ");
	console_write_hex64(far);
	console_write("\n");
	panic("unhandled synchronous exception");
}
