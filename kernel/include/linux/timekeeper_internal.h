/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TIMEKEEPER_INTERNAL_H
#define LKPI_LINUX_TIMEKEEPER_INTERNAL_H
#include <linux/clocksource.h>
#include <linux/ktime.h>
/* The layout KVM's master-clock code reads when the timekeeping core
 * announces a change (pvclock_gtod); that announcement never comes here. */
struct tk_read_base {
	struct clocksource *clock;
	u64 mask;
	u64 cycle_last;
	u32 mult;
	u32 shift;
	u64 xtime_nsec;
	ktime_t base;
	u64 base_real;
};
struct timekeeper {
	struct tk_read_base tkr_mono;
	struct tk_read_base tkr_raw;
	u64 xtime_sec;
	unsigned long ktime_sec;
	struct timespec64 wall_to_monotonic;
	ktime_t offs_real;
	ktime_t offs_boot;
	ktime_t offs_tai;
	u64 raw_sec;
};
#endif
