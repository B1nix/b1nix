/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_HRTIMER_H
#define LKPI_LINUX_HRTIMER_H
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/types.h>

/*
 * High-resolution timers.
 *
 * b1nix's timers are driven by the scheduler tick, so the resolution is 10 ms
 * rather than the nanoseconds the name promises. That is the whole difference,
 * and it is why this forwards to <linux/timer.h> rather than pretending: a
 * driver arming a 500 µs timer here gets one tick, and code that measures its
 * own latency against the request will see the discrepancy rather than a
 * plausible lie.
 */
enum hrtimer_restart { HRTIMER_NORESTART = 0, HRTIMER_RESTART };
enum hrtimer_mode { HRTIMER_MODE_ABS = 0, HRTIMER_MODE_REL = 1,
                    HRTIMER_MODE_REL_PINNED = 1 };
enum hrtimer_base_type { CLOCK_MONOTONIC_BASE = 0 };

struct hrtimer {
	struct timer_list timer;
	enum hrtimer_restart (*function)(struct hrtimer *);
	u64 interval_ns;
};

void hrtimer_init(struct hrtimer *t, int clock_id, enum hrtimer_mode mode);
void hrtimer_start(struct hrtimer *t, ktime_t when, enum hrtimer_mode mode);
int hrtimer_cancel(struct hrtimer *t);
int hrtimer_try_to_cancel(struct hrtimer *t);
bool hrtimer_active(const struct hrtimer *t);
static inline void hrtimer_start_range_ns(struct hrtimer *t, ktime_t when,
                                          u64 range_ns, enum hrtimer_mode mode)
{ (void)range_ns; hrtimer_start(t, when, mode); }
static inline u64 hrtimer_forward_now(struct hrtimer *t, ktime_t interval)
{ t->interval_ns = (u64)ktime_to_ns(interval); return 1; }
#endif
