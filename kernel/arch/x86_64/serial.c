#include <b1nix/io.h>
#include <b1nix/serial.h>
#include <b1nix/errno.h>

/* 16550 UART bases: COM1 backs the boot console/log, COM2 is an optional
 * second line (M39 serial getty). */
static const u16 serial_base[SERIAL_NPORTS] = {0x3f8, 0x2f8};
static int serial_detected[SERIAL_NPORTS];

/* Set while a line-configuration routine has DLAB raised on this port. With
 * DLAB set, register 0 is the low half of the baud divisor rather than the
 * transmit holding register — so a console character written in that window
 * would not go out on the wire, it would silently reprogram the baud rate.
 * COM1 is the boot console and is written from every CPU, so the window has
 * to exclude TX. The flag is checked, never waited on unboundedly: the window
 * is a handful of port writes, and a serial console must not be able to wedge
 * a panicking CPU. */
static volatile int serial_line_busy[SERIAL_NPORTS];

static void serial_hw_init(u16 base)
{
	outb(base + 1, 0x00);
	outb(base + 3, 0x80);
	outb(base + 0, 0x03);
	outb(base + 1, 0x00);
	outb(base + 3, 0x03);
	outb(base + 2, 0xc7);
	outb(base + 4, 0x0b);
}

void serial_init(void)
{
	/* COM1 is assumed present: it is the console mirror and the TX path
	 * already tolerates a dead port via the putc timeout. */
	serial_hw_init(serial_base[0]);
	serial_detected[0] = 1;

	/* Probe COM2 through the scratch register before trusting it. */
	outb(serial_base[1] + 7, 0xa5);
	if (inb(serial_base[1] + 7) == 0xa5) {
		outb(serial_base[1] + 7, 0x5a);
		if (inb(serial_base[1] + 7) == 0x5a) {
			serial_hw_init(serial_base[1]);
			serial_detected[1] = 1;
		}
	}
}

int serial_port_present(int idx)
{
	if (idx < 0 || idx >= SERIAL_NPORTS)
		return 0;
	return serial_detected[idx];
}

/* ~1 ms at 3 GHz; a character at 115200 baud takes 87 us. */
#define SERIAL_TX_WAIT_CYCLES 3000000ULL

static inline u64 serial_tsc(void) {
	unsigned lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

void serial_port_putc(int idx, char ch)
{
	if (idx < 0 || idx >= SERIAL_NPORTS || !serial_detected[idx])
		return;
	/* Wait for the transmitter by the clock, not by a loop count.
	 *
	 * A hundred thousand status reads is a hundred thousand port accesses,
	 * and under a hypervisor each of those is an exit — so a UART that
	 * stalls (the host applying backpressure to the log it is writing) held
	 * this CPU, and the console lock with it, for seconds. Other CPUs then
	 * declared a lockup on a lock that was merely slow. A millisecond is
	 * far longer than a 115200-baud character needs; past it, the character
	 * is dropped rather than the machine. */
	u64 deadline = serial_tsc() + SERIAL_TX_WAIT_CYCLES;
	do {
		if (!serial_line_busy[idx] && (inb(serial_base[idx] + 5) & 0x20)) {
			outb(serial_base[idx], (u8)ch);
			return;
		}
	} while (serial_tsc() < deadline);
	/* Real hardware may have no usable UART; output must not wedge. */
	(void)ch;
}

int serial_port_has_data(int idx)
{
	if (idx < 0 || idx >= SERIAL_NPORTS || !serial_detected[idx])
		return 0;
	return inb(serial_base[idx] + 5) & 1;
}

char serial_port_getc(int idx)
{
	if (!serial_port_has_data(idx))
		return 0;
	/* Same window as the TX side: with DLAB raised, register 0 reads back the
	 * divisor, not a received byte. */
	if (serial_line_busy[idx])
		return 0;
	return (char)inb(serial_base[idx]);
}

/* ── M109: programmable line settings ─────────────────────────────────────
 * The 16550's baud rate is a divisor of a fixed 115200 clock, held in a latch
 * that is only visible while DLAB (bit 7 of the LCR) is set. Everything else —
 * word length, parity, stop bits — lives in the low bits of the same LCR. */
#define UART_CLOCK 115200

int serial_port_set_line(int idx, u32 baud, u8 data_bits, u8 parity,
                         u8 stop_bits)
{
	if (idx < 0 || idx >= SERIAL_NPORTS || !serial_detected[idx])
		return -ENODEV;
	if (baud == 0 || UART_CLOCK % baud != 0)
		return -EINVAL;
	u32 divisor = UART_CLOCK / baud;
	if (divisor == 0 || divisor > 0xFFFF)
		return -EINVAL;
	if (data_bits < 5 || data_bits > 8 || parity > 2 ||
	    (stop_bits != 1 && stop_bits != 2))
		return -EINVAL;

	u8 lcr = (u8)(data_bits - 5);
	if (stop_bits == 2)
		lcr |= 0x04;
	if (parity == 1)
		lcr |= 0x08;             /* PEN, odd */
	else if (parity == 2)
		lcr |= 0x08 | 0x10;      /* PEN | EPS, even */

	u16 base = serial_base[idx];
	serial_line_busy[idx] = 1;
	outb(base + 3, 0x80);                    /* DLAB on */
	outb(base + 0, (u8)(divisor & 0xFF));
	outb(base + 1, (u8)(divisor >> 8));
	outb(base + 3, lcr);                     /* DLAB off, new word format */
	serial_line_busy[idx] = 0;
	return 0;
}

int serial_port_get_line(int idx, u32 *baud, u8 *data_bits, u8 *parity,
                         u8 *stop_bits)
{
	if (idx < 0 || idx >= SERIAL_NPORTS || !serial_detected[idx])
		return -1;
	u16 base = serial_base[idx];
	serial_line_busy[idx] = 1;
	u8 lcr = inb(base + 3);
	outb(base + 3, (u8)(lcr | 0x80));
	u32 divisor = (u32)inb(base + 0) | ((u32)inb(base + 1) << 8);
	outb(base + 3, lcr);
	serial_line_busy[idx] = 0;

	if (baud)
		*baud = divisor ? UART_CLOCK / divisor : 0;
	if (data_bits)
		*data_bits = (u8)(5 + (lcr & 0x03));
	if (parity)
		*parity = (lcr & 0x08) ? ((lcr & 0x10) ? 2 : 1) : 0;
	if (stop_bits)
		*stop_bits = (lcr & 0x04) ? 2 : 1;
	return 0;
}

u8 serial_port_get_mcr(int idx)
{
	if (idx < 0 || idx >= SERIAL_NPORTS || !serial_detected[idx])
		return 0;
	return inb(serial_base[idx] + 4);
}

void serial_port_set_mcr(int idx, u8 mcr)
{
	if (idx < 0 || idx >= SERIAL_NPORTS || !serial_detected[idx])
		return;
	/* OUT2 gates the interrupt line on a PC; never let a caller drop it, or
	 * the port goes deaf until the next reset. */
	outb(serial_base[idx] + 4, (u8)(mcr | 0x08));
}

u8 serial_port_get_msr(int idx)
{
	if (idx < 0 || idx >= SERIAL_NPORTS || !serial_detected[idx])
		return 0;
	return inb(serial_base[idx] + 6);
}

u16 serial_port_base(int idx)
{
	if (idx < 0 || idx >= SERIAL_NPORTS || !serial_detected[idx])
		return 0;
	return serial_base[idx];
}

void serial_putc(char ch)
{
	serial_port_putc(0, ch);
}

int serial_has_data(void)
{
	return serial_port_has_data(0);
}

char serial_getc(void)
{
	return serial_port_getc(0);
}

void serial_write(const char *text)
{
	for (usize i = 0; text[i] != '\0'; i++) {
		serial_putc(text[i]);
	}
}
