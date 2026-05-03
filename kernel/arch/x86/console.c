#include <tinyunix/console.h>
#include <tinyunix/serial.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile u16 *)0xb8000)

static usize cursor_row;
static usize cursor_col;
static u8 current_color = 0x0f;

static u16 vga_entry(char ch)
{
	return (u16)ch | ((u16)current_color << 8);
}

static void console_scroll(void)
{
	volatile u16 *vga = VGA_MEMORY;

	for (usize row = 1; row < VGA_HEIGHT; row++) {
		for (usize col = 0; col < VGA_WIDTH; col++) {
			vga[(row - 1) * VGA_WIDTH + col] = vga[row * VGA_WIDTH + col];
		}
	}

	for (usize col = 0; col < VGA_WIDTH; col++) {
		vga[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = vga_entry(' ');
	}

	cursor_row = VGA_HEIGHT - 1;
}

static void console_newline(void)
{
	cursor_col = 0;
	cursor_row++;

	if (cursor_row >= VGA_HEIGHT) {
		console_scroll();
	}
}

void console_init(void)
{
	volatile u16 *vga = VGA_MEMORY;
	cursor_row = 0;
	cursor_col = 0;

	for (usize row = 0; row < VGA_HEIGHT; row++) {
		for (usize col = 0; col < VGA_WIDTH; col++) {
			vga[row * VGA_WIDTH + col] = vga_entry(' ');
		}
	}
}

void console_putc(char ch)
{
	volatile u16 *vga = VGA_MEMORY;

	if (ch == '\n') {
		serial_putc(ch);
		console_newline();
		return;
	}

	vga[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(ch);
	serial_putc(ch);
	cursor_col++;

	if (cursor_col >= VGA_WIDTH) {
		console_newline();
	}
}

void console_write(const char *text)
{
	for (usize i = 0; text[i] != '\0'; i++) {
		console_putc(text[i]);
	}
}

void console_write_hex32(u32 value)
{
	const char *digits = "0123456789abcdef";

	for (int shift = 28; shift >= 0; shift -= 4) {
		console_putc(digits[(value >> shift) & 0xf]);
	}
}

void console_write_hex64(u64 value)
{
	const char *digits = "0123456789abcdef";

	for (int shift = 60; shift >= 0; shift -= 4) {
		console_putc(digits[(value >> shift) & 0xf]);
	}
}
