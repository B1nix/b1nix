/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PRINTK_H
#define LKPI_LINUX_PRINTK_H

#include <linux/compiler.h>
#include <linux/stdarg.h>

/* A format string bundled with its arguments, so a caller can pass "the rest of
 * this message" through one %pV. The DRM printers are built on it. */
struct va_format {
	const char *fmt;
	va_list *va;
};

int lkpi_vprintk(const char *fmt, va_list args);

/*
 * Kernel logging. Imported source annotates every message with a level prefix
 * inside the format string; b1nix's console does not parse those, so they are
 * passed through and appear literally, which is honest about where the line
 * came from and costs nothing.
 */
int lkpi_printk(const char *fmt, ...) __printf(1, 2);

/*
 * The level prefixes, in the shape imported code parses rather than a
 * readable one: a message's level is an ASCII SOH followed by the digit, and
 * printk_get_level() below recognises exactly that. Spelling them "<6>"
 * instead reads fine in a log and is invisible to that parser — btrfs then
 * labels every message with its default level, because none of them appears
 * to carry one. lkpi_printk strips the header before the line is written.
 */
#define KERN_SOH        "\001"
#define KERN_SOH_ASCII  '\001'
#define KERN_EMERG   KERN_SOH "0"
#define KERN_ALERT   KERN_SOH "1"
#define KERN_CRIT    KERN_SOH "2"
#define KERN_ERR     KERN_SOH "3"
#define KERN_WARNING KERN_SOH "4"
#define KERN_NOTICE  KERN_SOH "5"
#define KERN_INFO    KERN_SOH "6"
#define KERN_DEBUG   KERN_SOH "7"
#define KERN_CONT    KERN_SOH "c"

#define PRINTK_MAX_SINGLE_HEADER_LEN 2

/* The level a format string carries, or 0 if it carries none. */
static inline int printk_get_level(const char *buffer)
{
	if (buffer[0] == KERN_SOH_ASCII && buffer[1]) {
		switch (buffer[1]) {
		case '0' ... '7':
		case 'c':
			return buffer[1];
		}
	}
	return 0;
}

static inline const char *printk_skip_level(const char *buffer)
{
	return printk_get_level(buffer) ? buffer + 2 : buffer;
}

static inline const char *printk_skip_headers(const char *buffer)
{
	while (printk_get_level(buffer))
		buffer = printk_skip_level(buffer);
	return buffer;
}

#define printk(fmt, ...) lkpi_printk(fmt, ##__VA_ARGS__)
/* What imported code calls once the level has been split out of the format. */
#define _printk(fmt, ...) lkpi_printk(fmt, ##__VA_ARGS__)
#define pr_emerg(fmt, ...)   lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)   lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)    lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)     lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)    lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)  lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)   ((void)0)
#define pr_cont(fmt, ...)    lkpi_printk(fmt, ##__VA_ARGS__)

/* Dump a buffer as hex. Used by the EDID paths when a blob fails to parse, so
 * the bytes end up in the log rather than only a complaint about them. */
void print_hex_dump(const char *level, const char *prefix, int prefix_type,
                    int rowsize, int groupsize, const void *buf, usize len,
                    _Bool ascii);
#define DUMP_PREFIX_NONE   0
#define DUMP_PREFIX_ADDRESS 1
#define DUMP_PREFIX_OFFSET 2

#define no_printk(fmt, ...) ((void)0)


/* Print once and never again, however many times the call is reached. The flag
 * is per call site, which is what makes it useful for a message a driver would
 * otherwise emit per device or per frame. */
#define printk_once(fmt, ...)                                    \
	do {                                                         \
		static bool __printed;                                   \
		if (!__printed) { __printed = true; printk(fmt, ##__VA_ARGS__); } \
	} while (0)
#define pr_info_once(fmt, ...)  printk_once(fmt, ##__VA_ARGS__)
#define pr_warn_once(fmt, ...)  printk_once(fmt, ##__VA_ARGS__)
#define pr_err_once(fmt, ...)   printk_once(fmt, ##__VA_ARGS__)
#define pr_notice_once(fmt, ...) printk_once(fmt, ##__VA_ARGS__)

/* Upstream throttles a global message rate here; b1nix has no such governor,
 * so every caller is allowed to print. The callers are error paths that are
 * rare unless something is badly wrong, and then the messages are wanted. */
static inline int printk_ratelimit(void) { return 1; }

#endif
