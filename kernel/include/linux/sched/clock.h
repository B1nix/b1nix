/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SCHED_CLOCK_H
#define LKPI_LINUX_SCHED_CLOCK_H
#include <linux/types.h>
#include <lkpi/env.h>

/*
 * A monotonic nanosecond clock, cheap enough to read on a hot path.
 *
 * From the TSC, for the same reason ktime is: this is not only used to
 * timestamp events, it is used to bound busy-wait loops. i915's _wait_for_atomic
 * takes its deadline from here, and its shortest deadlines are microseconds —
 * on a clock that moves in 10 ms steps such a wait runs until the next tick,
 * which is five thousand times longer than asked. That is not merely imprecise:
 * a driver whose hardware happens to answer inside the accidental window works,
 * and stops working the moment the clock is made accurate.
 */
static inline u64 local_clock(void)
{
	return lkpi_monotonic_ns();
}

static inline u64 sched_clock(void) { return local_clock(); }
#endif
