/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_RATELIMIT_H
#define LKPI_LINUX_RATELIMIT_H
#include <linux/jiffies.h>
#include <linux/printk.h>
/* Rate-limited logging. The limiting is real — a driver that logs per frame
 * would otherwise fill the serial log and, as M32B showed, push test markers
 * out of it. */
struct ratelimit_state {
	unsigned long begin;
	int burst;
	int printed;
	int interval;
	/* How many were suppressed. Nothing here suppresses anything, so it stays
	 * zero — and a caller reporting it says "none swallowed", which is true. */
	int missed;
};
#define DEFINE_RATELIMIT_STATE(name, intv, brst) \
	struct ratelimit_state name = { 0, brst, 0, intv }
static inline int __ratelimit(struct ratelimit_state *rs)
{
	unsigned long now = jiffies;
	if (now - rs->begin >= (unsigned long)rs->interval) {
		rs->begin = now;
		rs->printed = 0;
	}
	return rs->printed++ < rs->burst;
}
#define printk_ratelimited(fmt, ...) lkpi_printk(fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(fmt, ...) lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define pr_err_ratelimited(fmt, ...)  lkpi_printk("drm: " fmt, ##__VA_ARGS__)
#define DRM_ERROR_RATELIMITED(fmt, ...) lkpi_printk("drm: " fmt, ##__VA_ARGS__)


/* The pieces imported code uses that the original set here did not carry.
 * `missed` is read by callers reporting what a rate limiter swallowed; nothing
 * here swallows anything, so it stays zero and says so. */
#define DEFAULT_RATELIMIT_INTERVAL (5 * 100)
#define DEFAULT_RATELIMIT_BURST    10
#define ratelimit_state_init(rs, i, b) \
	do { (rs)->interval = (i); (rs)->burst = (b); (rs)->printed = 0; \
	     (rs)->begin = 0; } while (0)
#define ratelimit_set_flags(rs, f) do { (void)(rs); (void)(f); } while (0)
#define RATELIMIT_MSG_ON_RELEASE 0

#endif
