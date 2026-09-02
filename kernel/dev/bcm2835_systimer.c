/*
 * The BCM2835 system timer: a free-running 64-bit counter at a fixed 1 MHz,
 * plus four compare registers the SoC raises interrupts from.
 *
 * The counter is what this kernel wants. A Raspberry Pi's ARM generic timer is
 * driven by a clock the firmware programs, and reading this one costs nothing
 * and cannot be reprogrammed out from under anybody — so it is the reference a
 * driver uses to check how long something actually took.
 *
 * The two halves are separate registers, so a read has to survive the low half
 * wrapping between them. Reading high, low, high again and repeating while the
 * high half moved is the standard way and is what this does.
 */

#include <b1nix/bcm2835.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/types.h>

#if defined(__aarch64__)

#define SYSTIMER_CLO 0x04
#define SYSTIMER_CHI 0x08

static u64 g_systimer;

static inline u32 st_read(u32 off)
{
	return *(volatile u32 *)(usize)(g_systimer + off);
}

u64 bcm2835_systimer_us(void)
{
	u32 hi, lo, hi2;

	if (!g_systimer)
		return 0;
	do {
		hi = st_read(SYSTIMER_CHI);
		lo = st_read(SYSTIMER_CLO);
		hi2 = st_read(SYSTIMER_CHI);
	} while (hi != hi2);
	return ((u64)hi << 32) | lo;
}

void bcm2835_systimer_init(void)
{
	g_systimer = fdt_systimer_base();
	if (!g_systimer)
		return;
	console_write("systimer: BCM2835 1 MHz counter at 0x");
	console_write_hex64(g_systimer);
	console_write("\n");
}

/*
 * Test mode: the counter has to move, and it has to move at roughly the rate
 * it claims. A stuck register reads the same value twice; a register that is
 * not this counter at all moves at some other rate. Busy-waiting on the ARM
 * generic timer for a known interval and comparing is what separates the two.
 */
void bcm2835_systimer_selftest(void)
{
	u64 t0, t1, elapsed;
	u64 freq, start, target;

	if (!g_systimer)
		return;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	if (!freq) {
		console_write("M109-RPI: FAIL systimer (no reference clock)\n");
		return;
	}

	t0 = bcm2835_systimer_us();
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
	target = start + freq / 100;   /* 10 ms */
	for (;;) {
		u64 now;

		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
		if (now >= target)
			break;
	}
	t1 = bcm2835_systimer_us();
	elapsed = t1 - t0;

	/* 10 ms is 10000 ticks of a 1 MHz counter. Half to double that is a wide
	 * enough window for an emulated clock and a narrow enough one to catch a
	 * counter running at the wrong rate or not at all. */
	if (elapsed >= 5000 && elapsed <= 20000) {
		console_write("M109-RPI: ok systimer ");
		console_write_dec(elapsed);
		console_write(" us\n");
	} else {
		console_write("M109-RPI: FAIL systimer ");
		console_write_dec(elapsed);
		console_write(" us\n");
	}
}

#else

void bcm2835_systimer_init(void) {}
void bcm2835_systimer_selftest(void) {}
u64 bcm2835_systimer_us(void) { return 0; }

#endif
