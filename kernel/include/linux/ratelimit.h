/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_RATELIMIT_H
#define LKPI_LINUX_RATELIMIT_H
#include <linux/jiffies.h>
#include <linux/printk.h>
/* Rate-limited logging. The limiting is real — a driver that logs per frame
 * would otherwise fill the serial log and, as M32B showed, push test markers
 * out of it. */
struct ratelimit_state { unsigned long begin; int burst; int printed; int interval; };
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
#endif
