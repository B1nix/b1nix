#include <string.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/io.h>
#include <b1nix/serial.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile u16 *)0xb8000)

static usize cursor_row;
static usize cursor_col;
static u8 current_color = 0x0f;

extern void fb_console_init(void);
extern void fb_console_write(const char *str);
extern void fb_console_putchar(char c);

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

void console_clear(void)
{
	if (bootinfo_get()->has_framebuffer) {
		extern void fb_console_clear(void);
		fb_console_clear();
	}

	volatile u16 *vga = VGA_MEMORY;
	for (usize row = 0; row < VGA_HEIGHT; row++) {
		for (usize col = 0; col < VGA_WIDTH; col++) {
			vga[row * VGA_WIDTH + col] = vga_entry(' ');
		}
	}
	cursor_row = 0;
	cursor_col = 0;
}

void console_putc(char ch)
{
	extern volatile u32 *fb_ptr;
	if (bootinfo_get()->has_framebuffer && fb_ptr) {
		fb_console_putchar(ch);
		serial_putc(ch);
		return;
	}

	volatile u16 *vga = VGA_MEMORY;

	if (ch == '\n') {
		serial_putc(ch);
		console_newline();
		return;
	}

	if (ch == '\b') {
		if (cursor_col > 0) {
			cursor_col--;
			vga[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(' ');
		}
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

void console_write_dec(u64 value)
{
	if (value == 0) {
		console_putc('0');
		return;
	}

	char buf[20];
	int i = 0;
	while (value > 0) {
		buf[i++] = '0' + (value % 10);
		value /= 10;
	}

	for (int j = i - 1; j >= 0; j--) {
		console_putc(buf[j]);
	}
}
