#include <b1nix/console.h>
#include <b1nix/serial.h>

void console_init(void)
{
	// For now, AArch64 just maps console output to serial.
}

void console_clear(void)
{
	// No-op for serial
}

void console_putc(char ch)
{
	serial_putc(ch);
}

void console_write(const char *text)
{
	serial_write(text);
}

void console_write_hex32(u32 value)
{
	const char *digits = "0123456789abcdef";
	for (int i = 28; i >= 0; i -= 4) {
		console_putc(digits[(value >> i) & 0xf]);
	}
}

void console_write_hex64(u64 value)
{
	const char *digits = "0123456789abcdef";
	for (int i = 60; i >= 0; i -= 4) {
		console_putc(digits[(value >> i) & 0xf]);
	}
}

void console_write_dec(u64 value)
{
	if (value == 0) {
		console_putc('0');
		return;
	}

	char buffer[20];
	int i = 0;
	while (value > 0) {
		buffer[i++] = '0' + (value % 10);
		value /= 10;
	}

	while (i > 0) {
		console_putc(buffer[--i]);
	}
}
