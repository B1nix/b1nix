/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SCHED_ISOLATION_H
#define LKPI_LINUX_SCHED_ISOLATION_H
#include <linux/types.h>
enum hk_type {
	HK_TYPE_DOMAIN,
	HK_TYPE_MANAGED_IRQ,
	HK_TYPE_KERNEL_NOISE,
	HK_TYPE_TIMER = HK_TYPE_KERNEL_NOISE,
	HK_TYPE_MAX
};
/* No isolcpus/nohz_full: every CPU does housekeeping. */
static inline bool housekeeping_enabled(enum hk_type type) { (void)type; return false; }
static inline bool housekeeping_cpu(int cpu, enum hk_type type) { (void)cpu; (void)type; return true; }
#endif
