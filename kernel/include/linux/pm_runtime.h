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

/* Take a reference without waiting for the resume to complete. b1nix's runtime
 * PM resumes synchronously inside the get, so this is the synchronous form —
 * the caller gets a device that is already up rather than one on its way up,
 * which is stronger than it asked for. */
static inline int pm_runtime_get(struct device *dev)
{ return lkpi_pm_runtime_get_sync(dev ? &dev->lk : 0); }

/* Take a reference only if the device is already resumed, reporting 1 when one
 * was taken and 0 when it was suspended. */
int pm_runtime_get_if_in_use(struct device *dev);

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


/* Tell the core this device's suspend is handled elsewhere. Nothing here acts
 * on it: b1nix's runtime PM is driven by the usage count alone. */
static inline void pm_runtime_no_callbacks(struct device *dev) { (void)dev; }


/* Let the core suspend this device when it goes idle. b1nix's runtime PM is
 * driven by the usage count alone and has no separate enable/allow split, so
 * allowing is what enabling already did. */
static inline void pm_runtime_allow(struct device *dev) { (void)dev; }
static inline void pm_runtime_forbid(struct device *dev) { (void)dev; }
static inline void pm_runtime_dont_use_autosuspend(struct device *dev) { (void)dev; }
static inline void pm_runtime_get_noresume(struct device *dev)
{ pm_runtime_get(dev); }
static inline int pm_runtime_put(struct device *dev)
{ return pm_runtime_put_autosuspend(dev); }

/* Take a reference only if the device is already active, reporting whether one
 * was taken. ign_usage_count selects whether a device held active purely by
 * other references counts; b1nix tracks only the usage count, so both spellings
 * answer from it. */
static inline int pm_runtime_get_if_active(struct device *dev, bool ign_usage_count)
{ (void)ign_usage_count; return pm_runtime_get_if_in_use(dev); }



#endif
