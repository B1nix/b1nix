#include <b1nix/console.h>
#include <stdio.h>
#include <b1nix/kmsg.h>
#include <b1nix/bootinfo.h>
#include <b1nix/kprintf.h>
#include <b1nix/ktime.h>
#include <b1nix/fb_console.h>
#include <b1nix/serial.h>
#include <b1nix/spinlock.h>
#include <b1nix/sched.h>

/* One writer at a time. Unlocked output was fine while one CPU existed; with
 * a second one running processes, two `write(1, …)` calls interleave inside a
 * single line and the harness greps for whole markers. x86_64 has had this
 * lock since its own SMP work. */
static spinlock_t console_lock = SPINLOCK_INIT;

static volatile int console_lock_owner;

static int console_this_cpu(void)
{
	struct percpu *pc = get_percpu();

	return pc ? pc->cpu_id : 0;
}

struct console_state console;
/* Bumped by every console_write / console_write_raw, exactly as on x86_64: the
 * in-guest silence watchdog decides a lane has wedged by watching this counter
 * stand still. It was defined here but never incremented, so on aarch64 the
 * watchdog fired every 60 s of a perfectly healthy run and dumped the task
 * table straight through whatever userspace was printing — which is how whole
 * markers ("BB-W6: ok passwd-unlock") came out cut in half and failed. */
volatile u64 g_console_write_seq = 0;

void console_init(void)
{
}

/* Panic path: whoever held the lock is not going to release it. */
void console_bust_lock(void)
{
	console_lock = 0;
	__atomic_store_n(&console_lock_owner, 0, __ATOMIC_RELEASE);
}

void console_clear(void)
{
}

/* Who holds it, so console_write can tell "somebody else is printing" from
 * "I am already inside my own section" -- the callers that take this lock
 * explicitly (pmm's fault reports, serial_tty) go on to call console_write,
 * and a non-recursive spinlock would have them wait for themselves. Stored as
 * cpu_id + 1 so 0 means unheld. */
int console_lock_held_here(void)
{
	return __atomic_load_n(&console_lock_owner, __ATOMIC_ACQUIRE) ==
	       console_this_cpu() + 1;
}

int console_lock_try_acquire_irqsave(u64 *flags)
{
	if (!spin_trylock_irqsave(&console_lock, flags))
		return 0;
	__atomic_store_n(&console_lock_owner, console_this_cpu() + 1,
	                 __ATOMIC_RELEASE);
	return 1;
}

void console_lock_acquire_irqsave(u64 *flags)
{
	spin_lock_irqsave(&console_lock, flags);
	__atomic_store_n(&console_lock_owner, console_this_cpu() + 1,
	                 __ATOMIC_RELEASE);
}

void console_lock_release_irqrestore(u64 flags)
{
	__atomic_store_n(&console_lock_owner, 0, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&console_lock, flags);
}

extern void klog_putc(char c);
extern void kmsg_putc(char c);

/* M107 virtual terminals and the framebuffer console, exactly as on x86: the
 * character is recorded in VT 1's cell buffer, drawn on the framebuffer when
 * this VT owns the display and no display server has claimed /dev/fb0, and
 * always echoed to serial - that is the boot and test transcript. */
int vt_console_putc(char c);
int vt_repaint_in_progress(void);
int fb_dev_claimed(void);
int fb_console_ready(void);
void fb_console_putchar(char c);

/* Draw on the framebuffer console when the VT layer is not going to.
 *
 * vt_console_putc() returns non-zero only when some OTHER virtual terminal
 * owns the display; zero means "this character belongs on screen" — including
 * before vt_init() has run at all, which is most of the boot. x86_64 has drawn
 * that case since the framebuffer console existed; this arch never did, so
 * every line went to klog, to the VT cell buffer and to the serial port, and
 * nothing whatsoever reached the panel. On a board whose only console IS the
 * panel — a phone with no serial cable — that is the difference between a
 * kernel log and a black screen. */
static void console_draw(char c)
{
	if (vt_console_putc(c))
		return;
	if (fb_console_ready() && !fb_dev_claimed())
		fb_console_putchar(c);
}

/* The `<N>` priority prefix kprintf puts at the head of every line.
 *
 * x86_64's console consumes it: the digit sets the record's priority, the three
 * characters never reach the screen, and a line below the console loglevel is
 * dropped. This console did none of that -- it printed the prefix literally and
 * ignored the level, so `<6>` and `<7>` were noise in every log (three of the
 * 45 columns a phone panel has), `loglevel=` on the command line did nothing at
 * all, and the M110 self-test that deliberately emits a FAIL marker at DEBUG
 * under an ERR console saw it come straight out.
 *
 * Suppression is about the console, not about the record: klog and kmsg get
 * every character either way, which is what the same self-test checks next.
 */
static int con_level_configured(void); /* defined with the loglevel state */

enum { CON_AT_BOL, CON_IN_PREFIX, CON_BODY };
static int con_state = CON_AT_BOL;
static int con_pfx_digit;
static int con_line_muted;

static void console_emit_raw(char c)
{
	klog_putc(c);
	kmsg_putc(c);
	if (con_line_muted)
		return;
	console_draw(c);
	serial_putc(c);
}

/* The same, minus the panel.
 *
 * A phone's panel holds about 45 columns at the magnification that makes it
 * readable at all, and "[    7.220000] " is fifteen of them -- a third of every
 * line spent on a number, on the one console where horizontal space is the
 * scarce resource and where there is no way to scroll back and correlate times
 * anyway. The stamp still goes to the log ring, /dev/kmsg and the serial port,
 * so dmesg and a cabled console are unchanged. */
static void console_emit_offscreen(char c)
{
	klog_putc(c);
	kmsg_putc(c);
	if (con_line_muted)
		return;
	serial_putc(c);
}

/* The bracketed monotonic timestamp every kernel log line opens with, in the
 * shape dmesg uses: "[    3.472918] ". Stamped when the line's first character
 * arrives, from ktime_monotonic_ns() -- the same clock /proc/uptime reports.
 * Without it aarch64 lines carried no time at all, so nothing in a log could
 * be ordered against anything else. */
static void console_emit_timestamp(void)
{
	u64 ns = ktime_monotonic_ns();
	u64 sec = ns / 1000000000ull;
	u64 usec = (ns % 1000000000ull) / 1000ull;
	char digits[24];
	int n = 0;

	console_emit_offscreen('[');
	if (sec == 0) {
		digits[n++] = '0';
	} else {
		while (sec > 0 && n < (int)sizeof(digits)) {
			digits[n++] = (char)('0' + (sec % 10));
			sec /= 10;
		}
	}
	for (int pad = n; pad < 5; pad++)
		console_emit_offscreen(' ');
	for (int i = n - 1; i >= 0; i--)
		console_emit_offscreen(digits[i]);
	console_emit_offscreen('.');
	for (u64 div = 100000; div > 0; div /= 10)
		console_emit_offscreen((char)('0' + (usec / div) % 10));
	console_emit_offscreen(']');
	console_emit_offscreen(' ');
}

static void console_sink(char c)
{
	switch (con_state) {
	case CON_AT_BOL:
		if (c == '<') {
			con_state = CON_IN_PREFIX;
			con_pfx_digit = -1;
			return;
		}
		con_state = CON_BODY;
		console_emit_timestamp();
		break;
	case CON_IN_PREFIX:
		if (con_pfx_digit < 0 && c >= '0' && c <= '9') {
			con_pfx_digit = c - '0';
			return;
		}
		if (con_pfx_digit >= 0 && c == '>') {
			con_line_muted = con_level_configured() &&
			                 con_pfx_digit > console_loglevel_get();
			kmsg_set_line_prio(con_pfx_digit);
			con_state = CON_BODY;
			console_emit_timestamp();
			return;
		}
		/* Not a prefix after all — put back what was swallowed. */
		con_state = CON_BODY;
		console_emit_timestamp();
		console_emit_raw('<');
		if (con_pfx_digit >= 0)
			console_emit_raw((char)('0' + con_pfx_digit));
		break;
	default:
		break;
	}

	console_emit_raw(c);
	if (c == '\n') {
		con_state = CON_AT_BOL;
		con_line_muted = 0;
	}
}

void console_putc(char c)
{
	console_sink(c);
}

void console_write_char(char c)
{
	console_putc(c);
}

/* Tick of the last console output. The test harness decides an instance has
 * hung from 120s of SILENCE, and then kills it — at which point every check it
 * had already passed is reported BLOCKED and the run tells you nothing. Track
 * the same signal in here so the kernel can end the instance cleanly and let
 * the harness read real results instead. */
volatile u64 g_last_console_tick;

/* One writer at a time, with interrupts masked.
 *
 * This console had no mutual exclusion at all -- console_write declared
 * `flags` and threw it away with a `(void)`, while the lock a few lines up
 * sat unused by everything except two callers that take it by hand. Two CPUs
 * (or a handler interrupting a writer) therefore interleaved character by
 * character: a line began without its timestamp because the stamp had gone out
 * in the middle of somebody else's line, and a panic dump -- the one thing that
 * has to be readable -- came out as two register dumps shuffled together. */
void console_write(const char *str)
{
	extern u64 scheduler_get_uptime_ticks(void);
	u64 flags;

	g_last_console_tick = scheduler_get_uptime_ticks();
	if (!str)
		return;
	g_console_write_seq++;
	if (console_lock_held_here()) {
		/* Already inside somebody's explicit console_lock_acquire_irqsave
		 * section -- pmm's fault reports and serial_tty do that to keep a
		 * multi-call block from being interleaved. Re-taking a
		 * non-recursive spinlock here would wedge that CPU on itself. */
		for (const char *p = str; *p; p++)
			console_sink(*p);
		return;
	}
	console_lock_acquire_irqsave(&flags);
	for (const char *p = str; *p; p++)
		console_sink(*p);
	console_lock_release_irqrestore(flags);
}

void console_write_dec(u64 val)
{
	if (val == 0) {
		console_write_char('0');
		return;
	}
	char buf[32];
	int i = 0;
	while (val > 0) {
		buf[i++] = '0' + (val % 10);
		val /= 10;
	}
	while (i > 0) {
		console_write_char(buf[--i]);
	}
}

void console_write_hex32(u32 val)
{
	const char *hex = "0123456789abcdef";
	for (int i = 7; i >= 0; i--) {
		console_write_char(hex[(val >> (i * 4)) & 0xf]);
	}
}

void console_write_hex64(u64 val)
{
	const char *hex = "0123456789abcdef";
	for (int i = 15; i >= 0; i--) {
		console_write_char(hex[(val >> (i * 4)) & 0xf]);
	}
}

/* Terminal output: no timestamp, no severity prefix, no filter -- /dev/console
 * and /dev/tty writes and the VT's keyboard echo. A shell prompt is not a log
 * record; stamping it would put a timestamp in front of every echoed keystroke
 * and in the middle of every screen repaint. Still captured in the ring, so the
 * dmesg transcript keeps showing what the console showed. */
void console_putc_raw(char ch)
{
	klog_putc(ch);
	kmsg_putc(ch);
	console_draw(ch);
	/* Everything except a VT repaint. A switch repaints the incoming VT's
	 * saved cell buffer through this primitive, and on this arch the serial
	 * port IS the log: the replay put a hundred already-logged lines back
	 * into it, with no timestamp, because a repainted screen is not a log
	 * record. Three VT switches in the M107 suite put three copies of the log
	 * inside the log. x86_64 never had this because its console_putc_raw
	 * paints the display device only. */
	if (!vt_repaint_in_progress())
		serial_putc(ch);
}

void console_write_raw(const char *text)
{
	if (!text)
		return;
	g_console_write_seq++;
	for (const char *p = text; *p; p++)
		console_putc_raw(*p);
}

static int con_loglevel = LOGLEVEL_DEBUG;
static int con_configured;

/* Nothing is suppressed until the command line has been read: a boot that dies
 * before console_log_init must not have swallowed its own diagnosis. */
static int con_level_configured(void) { return con_configured; }

/* `loglevel=` / `quiet` from the kernel command line, exactly as on x86_64.
 * There is no early buffer to flush here: this console writes straight to the
 * UART from the first character, so nothing was ever held back. */
void console_log_init(void)
{
	char value[16];
	int level = CONSOLE_LOGLEVEL_DEFAULT;

	if (bootinfo_has_flag("quiet"))
		level = CONSOLE_LOGLEVEL_QUIET;
	if (bootinfo_get_kv("loglevel", value, sizeof(value)) && value[0])
		level = (int)bootinfo_get_u32("loglevel", (u32)level);
	if (level < 0)
		level = 0;
	if (level > LOGLEVEL_DEBUG)
		level = LOGLEVEL_DEBUG;

	con_loglevel = level;
	con_configured = 1;
}

/* Asked before a line is formatted, so a suppressed debug line costs one
 * comparison. Everything is allowed through until the command line is read. */
int console_level_enabled(int level)
{
	if (!con_configured)
		return 1;
	return level <= con_loglevel;
}

/* A boot that dies before the command line is parsed must not take its own
 * diagnosis with it. */
void console_log_panic_flush(void)
{
	con_loglevel = LOGLEVEL_DEBUG;
	con_configured = 1;
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

