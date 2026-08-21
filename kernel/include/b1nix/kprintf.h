#ifndef B1NIX_KPRINTF_H
#define B1NIX_KPRINTF_H

#include <b1nix/types.h>

/* Structured kernel logging — the shape an operator expects from dmesg.
 *
 * A line produced through this interface looks like Linux's:
 *
 *     [    3.472918] pci 0000:00:02.0: virtio-gpu, BAR0 0xfe000000
 *
 * The bracketed monotonic timestamp comes from ktime_monotonic_ns() — the same
 * clock /proc/uptime reports. The severity is carried out of band as a
 * Linux-compatible "<N>" prefix on the text handed to the console, consumed by
 * the console line assembler, which decides whether the line reaches the
 * screen/serial (per the `loglevel=` boot parameter) and records the level in
 * the /dev/kmsg ring either way. Console and ring therefore always agree.
 *
 * Levels are Linux's, numerically: 0 is the loudest.
 */
#define LOGLEVEL_EMERG  0 /* system is unusable */
#define LOGLEVEL_ALERT  1 /* action must be taken immediately */
#define LOGLEVEL_CRIT   2 /* critical conditions */
#define LOGLEVEL_ERR    3 /* error conditions */
#define LOGLEVEL_WARNING 4 /* warning conditions */
#define LOGLEVEL_NOTICE 5 /* normal but significant */
#define LOGLEVEL_INFO   6 /* informational */
#define LOGLEVEL_DEBUG  7 /* debug-level messages */

/* What an unmarked console_write() line is assumed to be. Every legacy call
 * site lands here, which is what makes `quiet` meaningful without converting
 * all of them. */
#define LOGLEVEL_DEFAULT LOGLEVEL_INFO

/* Console filtering defaults, mirroring Linux. */
/* Info and above: debug-level chatter (three lines per exec from the ELF
 * loader alone) stays off a console that every write serialises on. */
#define CONSOLE_LOGLEVEL_DEFAULT 6 /* info and above */
#define CONSOLE_LOGLEVEL_QUIET   4 /* `quiet`: warnings and worse */

/* Emit one log line.
 *
 * `subsys` is the subsystem prefix and is printed as "subsys: message" — pass
 * a device-qualified string where there is one ("pci 0000:00:02.0", "ext4",
 * "tcp"), or NULL for a message that belongs to no subsystem. The trailing
 * newline is added here; do not put one in `fmt`. */
void kprintf(int level, const char *subsys, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#define k_emerg(subsys, ...)  kprintf(LOGLEVEL_EMERG, (subsys), __VA_ARGS__)
#define k_alert(subsys, ...)  kprintf(LOGLEVEL_ALERT, (subsys), __VA_ARGS__)
#define k_crit(subsys, ...)   kprintf(LOGLEVEL_CRIT, (subsys), __VA_ARGS__)
#define k_err(subsys, ...)    kprintf(LOGLEVEL_ERR, (subsys), __VA_ARGS__)
#define k_warn(subsys, ...)   kprintf(LOGLEVEL_WARNING, (subsys), __VA_ARGS__)
#define k_notice(subsys, ...) kprintf(LOGLEVEL_NOTICE, (subsys), __VA_ARGS__)
#define k_info(subsys, ...)   kprintf(LOGLEVEL_INFO, (subsys), __VA_ARGS__)
/* A debug line in a hot path costs a formatting pass even when nothing prints
 * it. Test the level first. The trade is deliberate: a suppressed debug line
 * is not built, so it is absent from dmesg too; a run that wants them asks
 * with loglevel=7. */
int console_level_enabled(int level);

#define k_dbg(subsys, ...)                                                     \
	do {                                                                   \
		if (console_level_enabled(LOGLEVEL_DEBUG))                     \
			kprintf(LOGLEVEL_DEBUG, (subsys), __VA_ARGS__);        \
	} while (0)

/* End-to-end check of the log format (timestamps, the loglevel filter and the
 * ring/console agreement). Runs under b1nix.test=1 only. */
void log_format_selftest(void);

/* Console severity filter, shared with syslog(2)'s CONSOLE_LEVEL. */
void console_log_init(void);
int console_loglevel_get(void);
void console_loglevel_set(int level);

#endif
