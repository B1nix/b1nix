#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/serial.h>
#include <b1nix/spinlock.h>

/* One writer at a time. Unlocked output was fine while one CPU existed; with
 * a second one running processes, two `write(1, …)` calls interleave inside a
 * single line and the harness greps for whole markers. x86_64 has had this
 * lock since its own SMP work. */
static spinlock_t console_lock = SPINLOCK_INIT;

struct console_state console;
u64 g_console_write_seq = 0;

void console_init(void)
{
}

/* Panic path: whoever held the lock is not going to release it. */
void console_bust_lock(void)
{
	console_lock = 0;
}

void console_clear(void)
{
}

void console_lock_acquire_irqsave(u64 *flags)
{
	spin_lock_irqsave(&console_lock, flags);
}

void console_lock_release_irqrestore(u64 flags)
{
	spin_unlock_irqrestore(&console_lock, flags);
}

extern void klog_putc(char c);
extern void kmsg_putc(char c);

/* M107 virtual terminals and the framebuffer console, exactly as on x86: the
 * character is recorded in VT 1's cell buffer, drawn on the framebuffer when
 * this VT owns the display and no display server has claimed /dev/fb0, and
 * always echoed to serial - that is the boot and test transcript. */
int vt_console_putc(char c);
int fb_dev_claimed(void);

void console_putc(char c)
{
	klog_putc(c);
	kmsg_putc(c);
	vt_console_putc(c);
	serial_putc(c);
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

void console_write(const char *str)
{
	extern u64 scheduler_get_uptime_ticks(void);
	u64 flags;

	g_last_console_tick = scheduler_get_uptime_ticks();
	(void)flags;
	if (str) {
		for (const char *p = str; *p; p++) {
			klog_putc(*p);
			kmsg_putc(*p);
			vt_console_putc(*p);
		}
	}
	serial_write(str);
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
