/*
 * The kernel panic screen.
 *
 * A panic used to leave whatever the console had last drawn on the display,
 * usually half a register dump over the tail of the boot log. This paints a
 * screen that says what happened at a glance -- the otter, the reason, where,
 * which CPU and task, and the call chain -- and hands the rest of the display
 * to the console, so the full dump that follows is drawn underneath it.
 *
 * Nothing here decides what is printed: the serial port and the log ring get
 * exactly what they got before. The screen is an extra view of the same
 * panic.
 *
 * It runs on a dying kernel, so: no allocation, no locks, no scheduler, no
 * device calls (see fb_console_panic_begin). Every address it follows is
 * range-checked first, and a fault while painting is caught by the guard in
 * panic_screen_show and turns the screen off instead of recursing.
 */
#include <stdio.h>
#include <string.h>
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/klog.h>
#include <b1nix/ktime.h>
#include <b1nix/lapic.h>
#include <b1nix/panic_screen.h>
#include <b1nix/sched.h>
#include <b1nix/serial.h>

#define PS_BACKGROUND 0x0f1117u
#define PS_CARD       0x2a3040u /* behind the otter, so its black outline reads */
#define PS_TITLE      0xff5a4du
#define PS_RULE       0xe30805u
#define PS_LABEL      0x8a93a6u
#define PS_VALUE      0xeef1f5u
#define PS_FRAME      0xffc66du
#define PS_TEXT       0xc3c9d4u

#define PS_TITLE_TEXT "B1NIX KERNEL PANIC"
#define PS_MAX_FRAMES 10
#define PS_REASON_LINES 3
#define PS_LINE_MAX 160

extern char __kernel_text_start[], __kernel_text_end[];

static char ps_fault[96];

void panic_screen_fault(const char *detail)
{
	usize n = 0;

	if (detail)
		for (; detail[n] && n < sizeof(ps_fault) - 1; n++)
			ps_fault[n] = detail[n];
	ps_fault[n] = 0;
}

static int ps_is_text(u64 addr)
{
	return addr >= (u64)(usize)__kernel_text_start &&
	       addr < (u64)(usize)__kernel_text_end;
}

/* The otter at `scale` screen pixels per art pixel, background left alone. */
static void ps_draw_otter(u32 x0, u32 y0, u32 scale)
{
	u32 w = panic_otter_width;
	u32 total = (u32)panic_otter_width * panic_otter_height;
	u32 pos = 0;

	for (usize i = 0; i < panic_otter_rle_len && pos < total; i++) {
		u32 idx = panic_otter_rle[i] >> 5;
		u32 run = (panic_otter_rle[i] & 0x1f) + 1u;

		if (run > total - pos)
			run = total - pos;
		if (idx == 0 || idx >= PANIC_OTTER_COLOURS) {
			pos += run;
			continue;
		}
		while (run) {
			u32 x = pos % w;
			u32 y = pos / w;
			u32 span = w - x < run ? w - x : run;

			fb_console_panic_fill(x0 + x * scale, y0 + y * scale,
			                      span * scale, scale,
			                      panic_otter_palette[idx]);
			pos += span;
			run -= span;
		}
	}
}

/* One line of at most `cols` characters; returns the next line's y. */
static u32 ps_line(u32 x, u32 y, const char *s, usize len, u32 cols,
                   u32 scale, u32 color)
{
	char buf[PS_LINE_MAX + 1];

	/* Wrapped, not cut: on a narrow panel the tail of a line is the part
	 * that says where (FAR, the file). Four rows at most. */
	if (cols > PS_LINE_MAX)
		cols = PS_LINE_MAX;
	if (!cols)
		return y + 10 * scale;
	for (int row = 0; row < 4; row++) {
		usize n = len > cols ? cols : len;

		memcpy(buf, s, n);
		buf[n] = '\0';
		fb_console_panic_text(x, y, buf, scale, color);
		y += 10 * scale;
		s += n;
		len -= n;
		if (!len)
			break;
	}
	return y;
}

/* "LABEL  value" on one line. */
static u32 ps_field(u32 x, u32 y, const char *label, const char *value,
                    u32 cols, u32 scale)
{
	usize n = strlen(label);
	u32 vx;

	ps_line(x, y, label, n, cols, scale, PS_LABEL);
	vx = x + (u32)(n + 1) * 8 * scale;
	if (n + 1 >= cols)
		return y + 10 * scale;
	return ps_line(vx, y, value, strlen(value), cols - (u32)(n + 1), scale,
	               PS_VALUE);
}

/* Frame-pointer chain, both architectures: [fp] = caller's fp, [fp+8] =
 * return address. Stops at anything that does not look like the next frame
 * of the same stack. */
static int ps_next_frame(u64 *fp, u64 *ret, u64 lo, u64 hi)
{
	const u64 *f;
	u64 next;

	if (!*fp || (*fp & 7) || *fp < lo || *fp + 16 > hi)
		return 0;
	f = (const u64 *)(usize)*fp;
	*ret = f[1];
	next = f[0];
	if (!ps_is_text(*ret))
		return 0;
	*fp = (next > *fp && next - *fp < KERNEL_STACK_SIZE) ? next : 0;
	return 1;
}

static void ps_format_frame(char *buf, usize size, int depth, u64 addr)
{
	u64 off = 0;
	const char *name = ksym_lookup(addr, &off);

	if (name)
		snprintf(buf, size, "#%d %s+0x%lx", depth, name, (unsigned long)off);
	else
		snprintf(buf, size, "#%d 0x%016lx", depth, (unsigned long)addr);
}

static void ps_paint(const char *reason, const char *file, int line, u64 pc,
                     u64 fp)
{
	u32 w = fb_console_width();
	u32 h = fb_console_height();
	u32 u, m, pad, os, ts, col_x, col_w, cols, y, card_bottom, top;
	u32 art_w, art_h;
	char buf[PS_LINE_MAX + 1];
	struct percpu *pc_cpu = get_percpu();
	struct task *t = pc_cpu ? pc_cpu->cur_task : 0;
	u64 lo, hi;

	/* A portrait panel (a phone) stacks the otter above the text and sizes
	 * the text by its width alone: at one unit per 640x400 a 1080x2520 panel
	 * got 8-pixel type that no photograph of it could read. */
	int portrait = h > w + w / 4;

	/* One unit of text magnification per 640x400 of screen, or per 360
	 * pixels of width on a portrait one. */
	u = w / 640 < h / 400 ? w / 640 : h / 400;
	if (portrait)
		u = w / 360;
	if (u < 1)
		u = 1;
	m = 12 * u;
	pad = 6 * u;

	/* The largest whole-pixel otter that takes at most two fifths of the
	 * width and under half the height -- three fifths and a quarter when it
	 * sits on top. */
	os = (w * (portrait ? 3 : 2) / 5) / panic_otter_width;
	if ((portrait ? h / 4 : h * 9 / 20) / panic_otter_height < os)
		os = (portrait ? h / 4 : h * 9 / 20) / panic_otter_height;
	if (os < 1)
		os = 1;
	art_w = panic_otter_width * os;
	art_h = panic_otter_height * os;

	fb_console_panic_fill(0, 0, w, h, PS_BACKGROUND);
	{
		u32 card_x = portrait ? (w - art_w) / 2 - pad : m;

		fb_console_panic_fill(card_x, m, art_w + 2 * pad, art_h + 2 * pad,
		                      PS_CARD);
		ps_draw_otter(card_x + pad, m + pad, os);
	}
	card_bottom = m + art_h + 2 * pad;

	col_x = portrait ? m : m + art_w + 2 * pad + m;
	col_w = w > col_x + m ? w - col_x - m : 0;
	cols = col_w / (8 * u);

	ts = 3 * u;
	while (ts > 1 && (u32)strlen(PS_TITLE_TEXT) * 8 * ts > col_w)
		ts--;
	y = portrait ? card_bottom + m : m;
	fb_console_panic_text(col_x, y, PS_TITLE_TEXT, ts, PS_TITLE);
	y += 8 * ts + 3 * u;
	fb_console_panic_fill(col_x, y, col_w, 2 * u, PS_RULE);
	y += 8 * u;
	y = ps_line(col_x, y, "The kernel hit a fatal error and has stopped.",
	            46, cols, u, PS_LABEL);
	y += 6 * u;

	/* The reason, wrapped over a few lines. */
	y = ps_line(col_x, y, "REASON", 6, cols, u, PS_LABEL);
	if (!reason || !reason[0])
		reason = "(no message)";
	if (cols) {
		usize len = strlen(reason);

		for (int i = 0; i < PS_REASON_LINES && len; i++) {
			usize n = len > cols ? cols : len;

			y = ps_line(col_x, y, reason, n, cols, u, PS_VALUE);
			reason += n;
			len -= n;
		}
	}
	y += 4 * u;

	if (file && file[0]) {
		snprintf(buf, sizeof(buf), "%s:%d", file, line);
		y = ps_field(col_x, y, "AT    ", buf, cols, u);
	}
	if (t)
		snprintf(buf, sizeof(buf), "%u   PID %lu '%s'",
		         (unsigned)(pc_cpu ? pc_cpu->cpu_id : 0),
		         (unsigned long)t->id, t->name ? t->name : "none");
	else
		snprintf(buf, sizeof(buf), "%u   (no task)",
		         (unsigned)(pc_cpu ? pc_cpu->cpu_id : 0));
	y = ps_field(col_x, y, "CPU   ", buf, cols, u);
	if (ps_fault[0])
		y = ps_field(col_x, y, "FAULT ", ps_fault, cols, u);
	{
		u64 ns = ktime_monotonic_ns();

		snprintf(buf, sizeof(buf), "%lu.%03lus",
		         (unsigned long)(ns / 1000000000ull),
		         (unsigned long)(ns / 1000000ull % 1000ull));
		y = ps_field(col_x, y, "UPTIME", buf, cols, u);
	}
	y += 4 * u;

	/* The call chain, within the stack the walk starts on. */
	y = ps_line(col_x, y, "BACKTRACE", 9, cols, u, PS_LABEL);
	if (t && t->stack && fp >= (u64)(usize)t->stack &&
	    fp < (u64)(usize)t->stack + KERNEL_STACK_SIZE) {
		lo = (u64)(usize)t->stack;
		hi = lo + KERNEL_STACK_SIZE;
	} else {
		/* A boot or interrupt stack: bound the walk to a window above fp. */
		lo = fp & ~(u64)(KERNEL_STACK_SIZE - 1);
		hi = lo + 2 * (u64)KERNEL_STACK_SIZE;
	}
	{
		int depth = 0;
		u64 ret;

		if (pc && ps_is_text(pc)) {
			ps_format_frame(buf, sizeof(buf), depth++, pc);
			y = ps_line(col_x, y, buf, strlen(buf), cols, u, PS_FRAME);
		}
		while (depth < PS_MAX_FRAMES && ps_next_frame(&fp, &ret, lo, hi)) {
			ps_format_frame(buf, sizeof(buf), depth++, ret);
			y = ps_line(col_x, y, buf, strlen(buf), cols, u, PS_FRAME);
		}
		if (!depth)
			y = ps_line(col_x, y, "(no frames)", 11, cols, u, PS_LABEL);
	}

	top = (y > card_bottom ? y : card_bottom) + m / 2;
	y = ps_line(m, top, "Full report below and on the serial console.", 44,
	            w > 2 * m ? (w - 2 * m) / (8 * u) : 0, u, PS_LABEL);
	fb_console_panic_fill(m, y, w > 2 * m ? w - 2 * m : 0, u, PS_RULE);
	y += u + m / 2;

	/* The dump in readable type: scale u in portrait mode, or u-1 in landscape */
	fb_console_panic_region(y, portrait ? u : (u > 1 ? u - 1 : 1), PS_TEXT, PS_BACKGROUND);

	/* Replay recent kernel boot log so the lower console displays live context */
	{
		static char ps_klog_buf[4096];
		usize n = klog_read(ps_klog_buf, sizeof(ps_klog_buf));
		for (usize i = 0; i < n; i++) {
			fb_console_putchar(ps_klog_buf[i]);
		}
	}
}

void panic_screen_show(const char *reason, const char *file, int line, u64 pc,
                       u64 fp)
{
	/* 0 = never painted, cpu + 1 = that CPU is painting, -1 = finished. */
	static volatile int state;
	struct percpu *cpu = get_percpu();
	int me = (cpu ? (int)cpu->cpu_id : 0) + 1;
	int expect = 0;

	if (!__atomic_compare_exchange_n(&state, &expect, me, 0,
	                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		/* Back here on the CPU that is painting: the painting faulted and
		 * this is the nested panic. Hands off the display from now on. */
		if (expect == me) {
			fb_console_panic_abort();
			__atomic_store_n(&state, -1, __ATOMIC_RELEASE);
		}
		return;
	}

	/* Guarantee console is unmuted and flushed for serial diagnosis */
	console_log_panic_flush();

	/* Output unified Crash Card to serial console */
	console_write("\n================================================================================\n");
	console_write("KERNEL PANIC: ");
	console_write(reason ? reason : "(unknown error)");
	console_write("\n================================================================================\n");
	if (file && file[0]) {
		console_write("  LOCATION: ");
		console_write(file);
		console_write(":");
		console_write_dec((u64)line);
		console_write("\n");
	}
	if (ps_fault[0]) {
		console_write("  FAULT:    ");
		console_write(ps_fault);
		console_write("\n");
	}
	if (pc) {
		console_write("  PC:       0x");
		console_write_hex64(pc);
		ksym_print(pc);
		console_write("\n");
	}
	if (fp) {
		console_write("  FP:       0x");
		console_write_hex64(fp);
		console_write("\n");
	}
	{
		struct percpu *pc_cpu = get_percpu();
		struct task *t = pc_cpu ? pc_cpu->cur_task : 0;
		console_write("  CPU:      ");
		console_write_dec(pc_cpu ? (u64)pc_cpu->cpu_id : 0);
		if (t) {
			console_write(" | TASK: pid=");
			console_write_dec((u64)t->id);
			console_write(" ('");
			console_write(t->name ? t->name : "none");
			console_write("')");
		}
		console_write("\n");
	}
	console_write("--------------------------------------------------------------------------------\n");
	panic_screen_serial_banner();

	if (fb_console_panic_begin() == 0) {
		if (!fp)
			fp = (u64)(usize)__builtin_frame_address(0);
		ps_paint(reason, file, line, pc, fp);
		fb_console_present_all();
	}
	__atomic_store_n(&state, -1, __ATOMIC_RELEASE);
}

void panic_screen_serial_banner(void)
{
	serial_write("\n");
	for (const char *const *l = panic_otter_ascii; *l; l++) {
		serial_write(*l);
		serial_write("\n");
	}
	serial_write("\n");
}

void panic_screen_demo(void)
{
	char mode[16];

	mode[0] = '\0';
	if (!bootinfo_get_kv("b1nix.panic-demo", mode, sizeof(mode)) &&
	    !bootinfo_has_flag("b1nix.panic-demo"))
		return;
	if (!strcmp(mode, "fault")) {
		/* Low memory is identity-mapped during boot, so a NULL read would
		 * succeed; an address no page table maps cannot. */
		volatile u64 *bad = (volatile u64 *)(usize)0xdead000000000000ull;

		console_write("panic-demo: kernel fault on purpose\n");
		(void)*bad;
	}
	panic("panic-demo: requested with b1nix.panic-demo");
}
