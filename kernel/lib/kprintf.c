#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <b1nix/console.h>
#include <b1nix/klog.h>
#include <b1nix/kprintf.h>
#include <b1nix/ktime.h>
#include <b1nix/sched.h>

/* One line's worth of text. Longer messages are truncated rather than split,
 * so a record never straddles two /dev/kmsg entries. */
#define KPRINTF_LINE_MAX 480

void kprintf(int level, const char *subsys, const char *fmt, ...)
{
	char line[KPRINTF_LINE_MAX];
	usize pos = 0;

	if (level < LOGLEVEL_EMERG)
		level = LOGLEVEL_EMERG;
	if (level > LOGLEVEL_DEBUG)
		level = LOGLEVEL_DEBUG;

	/* The severity travels with the text as Linux's "<N>" prefix: the console
	 * line assembler strips it, so a caller can also hand-write one in a plain
	 * console_write() and get the same treatment. */
	line[pos++] = '<';
	line[pos++] = (char)('0' + level);
	line[pos++] = '>';

	if (subsys && subsys[0]) {
		usize len = strlen(subsys);
		if (len > sizeof(line) - pos - 3)
			len = sizeof(line) - pos - 3;
		memcpy(line + pos, subsys, len);
		pos += len;
		line[pos++] = ':';
		line[pos++] = ' ';
	}

	va_list args;
	va_start(args, fmt);
	int written = vsnprintf(line + pos, sizeof(line) - pos - 2, fmt, args);
	va_end(args);
	if (written > 0) {
		usize add = (usize)written;
		if (add > sizeof(line) - pos - 2)
			add = sizeof(line) - pos - 2;
		pos += add;
	}

	line[pos++] = '\n';
	line[pos] = '\0';

	console_write(line);
}

/* ── Boot-log self-test (b1nix.test=1) ─────────────────────────────────────
 *
 * Proves the properties the log format promises, end to end rather than by
 * inspection:
 *
 *   clock       the stamp on a line comes from the same monotonic clock
 *               /proc/uptime reports, and never runs backwards;
 *   filter      a line above the console loglevel does not reach the console
 *               and IS still recorded — the host script checks both halves,
 *               because only it can see what the console actually printed;
 *   subsystem   a prefixed line renders as "subsys: message".
 *
 * The suppressed line deliberately carries a FAIL marker: if the filter ever
 * stops working, that string turns up in the boot log and the host check trips
 * on it. Nothing here prints an "ok" it has not established. */
void log_format_selftest(void)
{
	u64 t0 = ktime_monotonic_ns();
	u64 t1 = ktime_monotonic_ns();
	u64 ticks_ns = scheduler_get_uptime_ticks() * 10000000ull;

	if (t1 >= t0)
		k_info(NULL, "M110-LOG: ok clock-monotonic");
	else
		k_err(NULL, "M110-LOG: FAIL clock-monotonic");

	/* The log stamp and /proc/uptime read one clock, so they can differ only
	 * by the tick the coarse source is quantised to (10 ms) plus whatever
	 * elapsed between the two reads. One second is a generous bound that
	 * still catches "these are two unrelated counters". */
	u64 diff = t1 > ticks_ns ? t1 - ticks_ns : ticks_ns - t1;
	if (diff < 1000000000ull)
		k_info(NULL, "M110-LOG: ok clock-uptime-agree");
	else
		k_err(NULL, "M110-LOG: FAIL clock-uptime-agree");

	int saved = console_loglevel_get();
	console_loglevel_set(LOGLEVEL_ERR);
	kprintf(LOGLEVEL_DEBUG, NULL,
	        "M110-LOG: FAIL filter-let-a-debug-line-through");
	console_loglevel_set(saved);

	/* The suppressed line must still be in the ring: suppression is about the
	 * console, not about the record. */
	{
		static char tail[8192];
		usize n = klog_read(tail, sizeof(tail));
		int found = 0;
		if (n >= 21) {
			for (usize i = 0; i + 21 <= n && !found; i++)
				if (memcmp(tail + i, "M110-LOG: FAIL filter", 21) == 0)
					found = 1;
		}
		if (found)
			k_info(NULL, "M110-LOG: ok filter-recorded");
		else
			k_err(NULL, "M110-LOG: FAIL filter-recorded");
	}

	k_info("m110-log", "subsystem prefix");
	k_info(NULL, "M110-LOG: done");
}
