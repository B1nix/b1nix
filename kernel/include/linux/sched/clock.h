/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SCHED_CLOCK_H
#define LKPI_LINUX_SCHED_CLOCK_H
#include <linux/types.h>
#include <lkpi/env.h>

/*
 * A monotonic nanosecond clock, cheap enough to read on a hot path.
 *
 * Backed by the scheduler's tick rather than the TSC: the tick is what b1nix
 * already keeps coherent across CPUs, and a driver reading this is timing its
 * own operations, not measuring the hardware. The resolution is therefore 10 ms
 * and it is stated here rather than implied, because code that timestamps two
 * events inside one tick will see them as simultaneous.
 */
static inline u64 local_clock(void)
{
	return lkpi_ticks() * 10ull * 1000ull * 1000ull;
}

static inline u64 sched_clock(void) { return local_clock(); }
#endif
