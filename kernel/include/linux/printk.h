/* SPDX-License-Identifier: MIT */
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

#define KERN_EMERG   "<0>"
#define KERN_ALERT   "<1>"
#define KERN_CRIT    "<2>"
#define KERN_ERR     "<3>"
#define KERN_WARNING "<4>"
#define KERN_NOTICE  "<5>"
#define KERN_INFO    "<6>"
#define KERN_DEBUG   "<7>"
#define KERN_CONT    ""

#define printk(fmt, ...) lkpi_printk(fmt, ##__VA_ARGS__)
#define pr_emerg(fmt, ...)   lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)   lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)    lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)     lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)    lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)  lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)   ((void)0)
#define pr_cont(fmt, ...)    lkpi_printk(fmt, ##__VA_ARGS__)
#define pr_warn_once(fmt, ...) lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_err_once(fmt, ...)  lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_info_once(fmt, ...) lkpi_printk("drm: " fmt, ##__VA_ARGS__)

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

#endif
