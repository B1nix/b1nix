#include <b1nix/io.h>
#include <b1nix/serial.h>

#define COM1 0x3f8

static int serial_ready(void)
{
	return (inb(COM1 + 5) & 0x20) != 0;
}

void serial_init(void)
{
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x80);
	outb(COM1 + 0, 0x03);
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03);
	outb(COM1 + 2, 0xc7);
	outb(COM1 + 4, 0x0b);
}

void serial_putc(char ch)
{
	while (!serial_ready()) {
	}

	outb(COM1, (u8)ch);
}

void serial_write(const char *text)
{
	for (usize i = 0; text[i] != '\0'; i++) {
		serial_putc(text[i]);
	}
}
