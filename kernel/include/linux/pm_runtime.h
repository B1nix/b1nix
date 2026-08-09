/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_PM_RUNTIME_H
#define LKPI_LINUX_PM_RUNTIME_H

#include <linux/device.h>
#include <lkpi/device.h>

/* Linux-named front for the runtime PM in <lkpi/device.h>. The counting and the
 * failure semantics live there; this only unwraps the device. */

static inline void pm_runtime_enable(struct device *dev)
{
	lkpi_pm_runtime_enable(dev ? &dev->lk : 0);
}

static inline void pm_runtime_disable(struct device *dev)
{
	lkpi_pm_runtime_disable(dev ? &dev->lk : 0);
}

static inline int pm_runtime_get_sync(struct device *dev)
{
	return lkpi_pm_runtime_get_sync(dev ? &dev->lk : 0);
}

static inline int pm_runtime_put_sync(struct device *dev)
{
	return lkpi_pm_runtime_put_sync(dev ? &dev->lk : 0);
}

/* Linux's async put; b1nix has no autosuspend timer, so it is the sync one.
 * Stated rather than hidden: a driver relying on the delay gets an immediate
 * suspend, which is correct but less lazy. */
static inline int pm_runtime_put_autosuspend(struct device *dev)
{
	return lkpi_pm_runtime_put_sync(dev ? &dev->lk : 0);
}

static inline void pm_runtime_mark_last_busy(struct device *dev) { (void)dev; }
static inline void pm_runtime_use_autosuspend(struct device *dev) { (void)dev; }
static inline void pm_runtime_set_autosuspend_delay(struct device *dev, int d)
{
	(void)dev;
	(void)d;
}

static inline int pm_runtime_suspended(struct device *dev)
{
	return lkpi_pm_runtime_suspended(dev ? &dev->lk : 0);
}

#endif
