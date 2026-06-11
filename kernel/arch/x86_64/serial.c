#include <b1nix/io.h>
#include <b1nix/serial.h>

/* 16550 UART bases: COM1 backs the boot console/log, COM2 is an optional
 * second line (M39 serial getty). */
static const u16 serial_base[SERIAL_NPORTS] = {0x3f8, 0x2f8};
static int serial_detected[SERIAL_NPORTS];

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

void serial_port_putc(int idx, char ch)
{
	if (idx < 0 || idx >= SERIAL_NPORTS || !serial_detected[idx])
		return;
	for (int i = 0; i < 100000; i++) {
		if (inb(serial_base[idx] + 5) & 0x20) {
			outb(serial_base[idx], (u8)ch);
			return;
		}
	}
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
	return (char)inb(serial_base[idx]);
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
