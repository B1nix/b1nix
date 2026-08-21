#include <string.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/fb.h>
#include <b1nix/io.h>
#include <b1nix/klog.h>
#include <b1nix/kmsg.h>
#include <b1nix/kprintf.h>
#include <b1nix/ktime.h>
#include <b1nix/vt.h>
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

static void console_dev_putc(char ch)
{
	/* M107 virtual terminals: the kernel console is VT 1. While another VT
	 * owns the display the character is still recorded in VT 1's cell buffer
	 * (so switching back repaints it) but must not be drawn. Serial is never
	 * suppressed — it is the boot and test transcript. */
	if (vt_console_putc(ch)) {
		serial_putc(ch);
		return;
	}
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

/* ── Line assembly: timestamps, severity and the loglevel filter ───────────
 *
 * Every character printed by the kernel passes through console_putc(), which
 * is where a line acquires the shape an operator expects:
 *
 *     [    3.472918] ext4: mounted /dev/sata0 at /
 *
 * The bracketed monotonic timestamp is stamped when the line's first character
 * arrives and comes from ktime_monotonic_ns(), the same clock /proc/uptime
 * reports. If the line opens with a Linux-style "<N>" severity prefix (what
 * kprintf() emits) the digit is consumed here: it sets the record's priority in
 * the /dev/kmsg ring and decides whether the line reaches the screen and the
 * serial port. A line without one is LOGLEVEL_DEFAULT, so the thousands of
 * plain console_write() call sites still get timestamps and still answer to
 * `quiet` without being converted one by one.
 *
 * Suppressed lines are only suppressed on the *console*: they are recorded in
 * the ring in full, exactly as they would have been printed, so dmesg and the
 * console never disagree about what a line said. */
enum console_line_state {
	CON_LINE_START = 0, /* nothing written on this line yet */
	CON_LINE_LT,        /* saw '<' */
	CON_LINE_LT_DIGIT,  /* saw "<N" */
	CON_LINE_BODY,      /* prefix decided, printing the message */
};

static enum console_line_state con_state = CON_LINE_START;
static int con_line_level = LOGLEVEL_DEFAULT;
static int con_line_muted;
static char con_pending_digit;

static int con_loglevel = CONSOLE_LOGLEVEL_DEFAULT;
static int con_configured; /* has the kernel command line been read yet? */

/* Output produced before the command line has been parsed cannot be filtered
 * yet — `quiet` is not known. It is held here and released by
 * console_log_init() once the loglevel is decided, or dumped unconditionally if
 * we crash first. Records are "\x01" + level digit + line text. */
#define CON_EARLY_BUF_SIZE 16384
static char con_early_buf[CON_EARLY_BUF_SIZE];
static usize con_early_len;

static void console_early_put(char ch)
{
	if (con_early_len < sizeof(con_early_buf))
		con_early_buf[con_early_len++] = ch;
}

/* Emit one character of an already-decided line to the console devices. */
static void console_line_out(char ch)
{
	if (con_line_muted)
		return;
	if (!con_configured) {
		console_early_put(ch);
		return;
	}
	console_dev_putc(ch);
}

/* Both sinks: the ring always, the devices only if the line is not filtered. */
static void console_line_emit(char ch)
{
	klog_putc(ch); /* the dmesg transcript */
	kmsg_putc(ch); /* and the /dev/kmsg record ring */
	console_line_out(ch);
}

static void console_emit_timestamp(void)
{
	u64 ns = ktime_monotonic_ns();
	u64 sec = ns / 1000000000ull;
	u64 usec = (ns % 1000000000ull) / 1000ull;
	char stamp[32];
	usize pos = 0;

	stamp[pos++] = '[';
	/* Seconds, right-aligned in five columns like Linux's "[    3.472918]". */
	char digits[24];
	int n = 0;
	if (sec == 0) {
		digits[n++] = '0';
	} else {
		while (sec > 0 && n < (int)sizeof(digits)) {
			digits[n++] = (char)('0' + (sec % 10));
			sec /= 10;
		}
	}
	for (int pad = n; pad < 5; pad++)
		stamp[pos++] = ' ';
	for (int i = n - 1; i >= 0; i--)
		stamp[pos++] = digits[i];
	stamp[pos++] = '.';
	for (u64 div = 100000; div > 0; div /= 10) {
		stamp[pos++] = (char)('0' + (usec / div) % 10);
		if (div == 1)
			break;
	}
	stamp[pos++] = ']';
	stamp[pos++] = ' ';

	for (usize i = 0; i < pos; i++)
		console_line_emit(stamp[i]);
}

/* Open a line at the decided severity: record the priority for the kmsg record
 * being assembled, apply the console filter, print the timestamp. */
static void console_line_begin(int level)
{
	con_line_level = level;
	con_line_muted = (!con_configured) ? 0 : (level > con_loglevel);
	con_state = CON_LINE_BODY;

	kmsg_set_line_prio(level);
	if (!con_configured) {
		console_early_put('\x01');
		console_early_put((char)('0' + level));
	}
	console_emit_timestamp();
}

static void console_line_end(void)
{
	console_line_emit('\n');
	con_state = CON_LINE_START;
	con_line_muted = 0;
	con_line_level = LOGLEVEL_DEFAULT;
}

void console_putc(char ch)
{
	switch (con_state) {
	case CON_LINE_START:
		if (ch == '<') {
			con_state = CON_LINE_LT;
			return;
		}
		console_line_begin(LOGLEVEL_DEFAULT);
		break;
	case CON_LINE_LT:
		if (ch >= '0' && ch <= '7') {
			con_pending_digit = ch;
			con_state = CON_LINE_LT_DIGIT;
			return;
		}
		/* Not a severity prefix after all — the '<' was message text. */
		console_line_begin(LOGLEVEL_DEFAULT);
		console_line_emit('<');
		break;
	case CON_LINE_LT_DIGIT:
		if (ch == '>') {
			console_line_begin(con_pending_digit - '0');
			return;
		}
		console_line_begin(LOGLEVEL_DEFAULT);
		console_line_emit('<');
		console_line_emit(con_pending_digit);
		break;
	case CON_LINE_BODY:
		break;
	}

	if (ch == '\n') {
		console_line_end();
		return;
	}
	console_line_emit(ch);
}

/* Raw console output: no timestamp, no severity, no filter.
 *
 * This is what a *terminal* write is — /dev/console and /dev/tty output, and
 * the VT's keyboard echo. A shell prompt is not a log record: stamping it would
 * put "[   12.345678] " in front of every echoed keystroke and in the middle of
 * every screen repaint. It is still captured in the ring, so the dmesg
 * transcript keeps showing what the console showed. */
extern volatile u64 g_console_write_seq;

void console_putc_raw(char ch)
{
	klog_putc(ch);
	kmsg_putc(ch);
	if (!con_configured) {
		console_early_put(ch);
		return;
	}
	console_dev_putc(ch);
}

void console_write_raw(const char *text)
{
	u64 flags;
	g_console_write_seq++;
	spin_lock_irqsave(&console_lock, &flags);
	for (usize i = 0; text[i] != '\0'; i++)
		console_putc_raw(text[i]);
	spin_unlock_irqrestore(&console_lock, flags);
}

/* Release everything printed before the command line was known. */
static void console_log_flush_early(void)
{
	usize i = 0;
	int muted = 0;

	while (i < con_early_len) {
		char ch = con_early_buf[i++];
		if (ch == '\x01' && i < con_early_len) {
			int level = con_early_buf[i++] - '0';
			muted = (level > con_loglevel);
			continue;
		}
		if (!muted)
			console_dev_putc(ch);
	}
	con_early_len = 0;
}

/* Read `loglevel=` and `quiet` from the kernel command line and let the console
 * start printing. Called from kernel_main() as soon as bootinfo is parsed. */
void console_log_init(void)
{
	u64 flags;

	char value[16];
	int level = CONSOLE_LOGLEVEL_DEFAULT;

	/* `quiet` lowers the console to warnings and worse, exactly as on Linux;
	 * an explicit `loglevel=` always wins over it. */
	if (bootinfo_has_flag("quiet"))
		level = CONSOLE_LOGLEVEL_QUIET;
	if (bootinfo_get_kv("loglevel", value, sizeof(value)) && value[0])
		level = (int)bootinfo_get_u32("loglevel", (u32)level);
	if (level < 0)
		level = 0;
	if (level > LOGLEVEL_DEBUG)
		level = LOGLEVEL_DEBUG;

	spin_lock_irqsave(&console_lock, &flags);
	con_loglevel = level;
	con_configured = 1;
	console_log_flush_early();
	spin_unlock_irqrestore(&console_lock, flags);
}

/* Would a line at this level be printed? Asked BEFORE the line is built, so a
 * suppressed debug line costs one comparison instead of a formatting pass and
 * a console call. Nothing can be filtered before the command line is read, so
 * everything is allowed through until then and held in the early buffer. */
int console_level_enabled(int level)
{
	if (!con_configured)
		return 1;
	return level <= con_loglevel;
}

int console_loglevel_get(void)
{
	return con_loglevel;
}

void console_loglevel_set(int level)
{
	if (level < 0)
		level = 0;
	if (level > LOGLEVEL_DEBUG)
		level = LOGLEVEL_DEBUG;
	con_loglevel = level;
}

/* Dump anything still held in the early buffer regardless of severity. The
 * crash paths call this: a boot that dies before the command line is parsed
 * must not take its own diagnosis with it. */
void console_log_panic_flush(void)
{
	con_loglevel = LOGLEVEL_DEBUG;
	con_configured = 1;
	con_line_muted = 0;
	console_log_flush_early();
}

/* Bumped on every console write. The scheduler's silence watchdog samples it:
 * a test instance that stops printing has either wedged or deadlocked, and
 * without an in-guest dump the only evidence is a truncated log. */
volatile u64 g_console_write_seq;

void console_write(const char *text)
{
	u64 flags;
	g_console_write_seq++;
	spin_lock_irqsave(&console_lock, &flags);
	for (usize i = 0; text[i] != '\0'; i++) {
		console_putc(text[i]);
	}
	spin_unlock_irqrestore(&console_lock, flags);
}

/* Forcibly release the console lock (bust_spinlocks pattern). Called from the
 * fatal exception path: a fault may have interrupted code on this CPU that held
 * the lock (e.g. a log/backtrace mid-print), and the handler's own
 * console_write would then self-deadlock. Best-effort — only used when we are
 * already crashing, so a momentarily garbled line on another CPU is acceptable. */
void console_bust_lock(void)
{
	console_lock = 0;
	console_log_panic_flush();
}

void console_lock_acquire_irqsave(u64 *flags)
{
	spin_lock_irqsave(&console_lock, flags);
}

void console_lock_release_irqrestore(u64 flags)
{
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
