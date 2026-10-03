/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PVCLOCK_GTOD_H
#define LKPI_LINUX_PVCLOCK_GTOD_H
#include <linux/notifier.h>
/* The timekeeping core never announces a clocksource change here: the TSC
 * this kernel keeps time with is fixed at boot. Registering succeeds and the
 * notifier is simply never called, as on a Linux machine whose clocksource
 * never changes. */
static inline int pvclock_gtod_register_notifier(struct notifier_block *nb)
{
	(void)nb;
	return 0;
}
static inline int pvclock_gtod_unregister_notifier(struct notifier_block *nb)
{
	(void)nb;
	return 0;
}
#endif
