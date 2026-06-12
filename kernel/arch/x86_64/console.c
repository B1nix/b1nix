#include <string.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/fb.h>
#include <b1nix/io.h>
#include <b1nix/klog.h>
#include <b1nix/serial.h>
#include <b1nix/spinlock.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile u16 *)0xb8000)

static usize cursor_row;
static usize cursor_col;
static u8 current_color = 0x0f;

struct console_state console;

/* Serialize console_write* across CPUs so multi-character outputs aren't
 * interleaved character-by-character on serial/VGA. Without this, every
 * concurrent klog_warn produced unreadable output like
 *   "[M2tem6Dpt=1I cAount=G]"
 * under SMP. Held only across the body of a single console_write call —
 * individual console_putc still goes through serial_putc without recursion. */
static spinlock_t console_lock = SPINLOCK_INIT;

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
	console.fg_pgrp = 1;

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
	klog_putc(ch);   /* capture every console char into the dmesg ring buffer */
	if (bootinfo_get()->has_framebuffer && fb_console_ready() &&
	    !fb_dev_claimed()) {
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
	u64 flags;
	spin_lock_irqsave(&console_lock, &flags);
	for (usize i = 0; text[i] != '\0'; i++) {
		console_putc(text[i]);
	}
	spin_unlock_irqrestore(&console_lock, flags);
}

void console_write_hex32(u32 value)
{
	const char *digits = "0123456789abcdef";

	u64 flags;
	spin_lock_irqsave(&console_lock, &flags);
	for (int shift = 28; shift >= 0; shift -= 4) {
		console_putc(digits[(value >> shift) & 0xf]);
	}
	spin_unlock_irqrestore(&console_lock, flags);
}

void console_write_hex64(u64 value)
{
	const char *digits = "0123456789abcdef";

	u64 flags;
	spin_lock_irqsave(&console_lock, &flags);
	for (int shift = 60; shift >= 0; shift -= 4) {
		console_putc(digits[(value >> shift) & 0xf]);
	}
	spin_unlock_irqrestore(&console_lock, flags);
}

void console_write_dec(u64 value)
{
	u64 flags;
	spin_lock_irqsave(&console_lock, &flags);
	if (value == 0) {
		console_putc('0');
		spin_unlock_irqrestore(&console_lock, flags);
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
	spin_unlock_irqrestore(&console_lock, flags);
}
