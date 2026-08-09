/* SPDX-License-Identifier: MIT */
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
#endif
