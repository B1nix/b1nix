#include <b1nix/io.h>
#include <b1nix/serial.h>

#define COM1 0x3f8

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
	for (int i = 0; i < 100000; i++) {
		if (inb(COM1 + 5) & 0x20) {
			outb(COM1, (u8)ch);
			return;
		}
	}
	(void)ch;
}

int serial_has_data(void)
{
	return inb(COM1 + 5) & 1;
}

char serial_getc(void)
{
	if (!serial_has_data()) return 0;
	return (char)inb(COM1);
}

void serial_write(const char *text)
{
	for (usize i = 0; text[i] != '\0'; i++) {
		serial_putc(text[i]);
	}
}
