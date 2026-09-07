/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BUG_H
#define LKPI_LINUX_BUG_H

#include <linux/printk.h>
#include <lkpi/env.h>
/* WARN reports and continues; BUG does not return. Keeping that difference is
 * the point: imported code uses WARN_ON for conditions it then handles, and
 * turning those into panics would take the kernel down for something the
 * driver was prepared for. */
/* Set while a panic is being printed, so code can avoid locks the panicking
 * CPU may already hold. b1nix's panic path does not set it, so it stays zero —
 * the safe answer, since callers then take the normal path. */
extern int oops_in_progress;

#define BUG() lkpi_panic("BUG in imported driver code")
#define BUG_ON(cond) do { if (cond) BUG(); } while (0)
#define WARN_ON(cond)                                     \
	({                                                    \
		int __c = !!(cond);                               \
		if (__c) lkpi_printk("drm: WARN_ON(%s)\n", #cond); \
		__c;                                              \
	})
#define WARN(cond, fmt, ...)                              \
	({                                                    \
		int __c = !!(cond);                               \
		if (__c) lkpi_printk("drm: " fmt, ##__VA_ARGS__); \
		__c;                                              \
	})
#define WARN_ONCE(cond, fmt, ...) WARN(cond, fmt, ##__VA_ARGS__)
#define WARN_ON_ONCE(cond) WARN_ON(cond)

/*
 * Taint flags.
 *
 * Linux records that something happened which makes a later bug report less
 * trustworthy — a warning fired, a driver forced a workaround — and prints the
 * accumulated set with every subsequent oops. b1nix has one kernel log and no
 * out-of-tree modules to blame, so the flags are recorded and reported through
 * the same log rather than kept in a separate register.
 */
#define TAINT_WARN            9
#define TAINT_MACHINE_CHECK   4
#define TAINT_DIE             1
#define LOCKDEP_STILL_OK      1
#define LOCKDEP_NOW_UNRELIABLE 0

void add_taint(unsigned flag, int lockdep_ok);


#define TAINT_USER            6
#define TAINT_FIRMWARE_WORKAROUND 11
#define TAINT_CRAP            10

/*
 * panic().
 *
 * A filesystem calls it when it finds an inconsistency it cannot continue past
 * — btrfs does when its `panic_on_error` mount option is set, and jbd2 does
 * when the journal is in a state that would corrupt the filesystem to ignore.
 * It has to be the real thing: continuing after one is how a single bad block
 * becomes an unmountable filesystem.
 */
void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* WARN, but not more often than the rate limiter allows. Same return value as
 * WARN_ON — the condition — so it can be used in an if. */
#define WARN_RATELIMIT(cond, fmt, ...) WARN_ON(cond)

#endif
