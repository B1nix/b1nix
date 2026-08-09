/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_TIME64_H
#define LKPI_LINUX_TIME64_H
#include <linux/ktime.h>
#include <linux/types.h>
/* Wall-ish time as seconds plus nanoseconds. The nanoseconds carry only tick
 * resolution here — 10 ms — because that is what <linux/ktime.h> can offer;
 * a vblank timestamp is therefore coarse, and says so rather than implying
 * precision it does not have. */
struct timespec64 {
	s64 tv_sec;
	long tv_nsec;
};
static inline struct timespec64 ktime_to_timespec64(ktime_t k)
{
	struct timespec64 ts;
	ts.tv_sec = k / NSEC_PER_SEC;
	ts.tv_nsec = (long)(k % NSEC_PER_SEC);
	return ts;
}
static inline ktime_t timespec64_to_ktime(struct timespec64 ts)
{
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}
static inline void ktime_get_ts64(struct timespec64 *ts)
{ *ts = ktime_to_timespec64(ktime_get()); }
#endif
