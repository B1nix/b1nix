/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TIME64_H
#define LKPI_LINUX_TIME64_H
#include <linux/ktime.h>
#include <linux/types.h>
/* Wall-ish time as seconds plus nanoseconds. The nanoseconds carry only tick
 * resolution here — 10 ms — because that is what <linux/ktime.h> can offer;
 * a vblank timestamp is therefore coarse, and says so rather than implying
 * precision it does not have. */
#ifndef LKPI_TIME64_T_DEFINED
#define LKPI_TIME64_T_DEFINED
typedef long long time64_t;
#endif

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
static inline bool timespec64_equal(const struct timespec64 *a,
                                    const struct timespec64 *b)
{ return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec; }

static inline int timespec64_compare(const struct timespec64 *lhs,
                                     const struct timespec64 *rhs)
{
	if (lhs->tv_sec < rhs->tv_sec)
		return -1;
	if (lhs->tv_sec > rhs->tv_sec)
		return 1;
	return (int)(lhs->tv_nsec - rhs->tv_nsec);
}

/* Clamp a timestamp to what the filesystem can store. An ext4 inode with
 * 128-byte inodes holds seconds only to 2038, and a value past that must be
 * pinned rather than wrapped — a wrapped mtime is a file from 1901. */
struct timespec64 timestamp_truncate(struct timespec64 t, struct inode *inode);

#endif
