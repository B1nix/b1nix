/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PM_H
#define LKPI_LINUX_PM_H
#include <linux/types.h>

/*
 * System-wide suspend/resume callbacks.
 *
 * Runtime PM — the part a GPU driver uses constantly — is real here and lives
 * in <linux/pm_runtime.h>. This is the other axis: whole-machine suspend to
 * RAM, which b1nix does not implement. The callback structures exist so a
 * driver can declare its handlers; nothing invokes them, because nothing here
 * ever suspends the machine.
 */
struct device;

struct dev_pm_ops {
	int (*prepare)(struct device *dev);
	void (*complete)(struct device *dev);
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
	int (*freeze)(struct device *dev);
	int (*thaw)(struct device *dev);
	int (*poweroff)(struct device *dev);
	int (*restore)(struct device *dev);
	int (*suspend_late)(struct device *dev);
	int (*resume_early)(struct device *dev);
	int (*runtime_suspend)(struct device *dev);
	int (*runtime_resume)(struct device *dev);
	int (*runtime_idle)(struct device *dev);
	/* The hibernation half. b1nix does not hibernate, so nothing calls these;
	 * they exist because a driver's ops table names them. */
	int (*freeze_late)(struct device *dev);
	int (*thaw_early)(struct device *dev);
	int (*poweroff_late)(struct device *dev);
	int (*restore_early)(struct device *dev);
	int (*restore_noirq)(struct device *dev);
	int (*suspend_noirq)(struct device *dev);
	int (*resume_noirq)(struct device *dev);
};

#define SET_SYSTEM_SLEEP_PM_OPS(susp, res) .suspend = susp, .resume = res,
#define SET_RUNTIME_PM_OPS(susp, res, idle) \
	.runtime_suspend = susp, .runtime_resume = res, .runtime_idle = idle,
#define SIMPLE_DEV_PM_OPS(name, susp, res) \
	const struct dev_pm_ops name = { .suspend = susp, .resume = res }

/* The event codes a suspend callback is handed. Defined for completeness of the
 * interface; nothing here generates them. */
typedef struct pm_message { int event; } pm_message_t;
#define PM_EVENT_SUSPEND 0x0002
#define PM_EVENT_FREEZE  0x0001

/* The name of the sysfs group a device's power attributes live in. */
extern const char power_group_name[];


/* The hibernation callbacks. b1nix does not hibernate; the members exist
 * because a driver's ops table names them. */


#define DPM_FLAG_NO_DIRECT_COMPLETE (1u << 0)
#define DPM_FLAG_SMART_PREPARE      (1u << 1)
#define DPM_FLAG_SMART_SUSPEND      (1u << 2)
#define DPM_FLAG_MAY_SKIP_RESUME    (1u << 3)

/* Hints to the PM core about how a driver's suspend interacts with its
 * children. b1nix's suspend path is a straight walk with no direct-complete
 * optimisation to opt out of, so the flags describe a behaviour that is
 * already the only one. */
struct device;
static inline void dev_pm_set_driver_flags(struct device *dev, u32 flags)
{ (void)dev; (void)flags; }


/*
 * The generic PM callbacks a bus type installs when it has nothing of its own
 * to do beyond forwarding to the driver. b1nix's PM core calls the driver
 * directly, so a bus that names these gets forwarding that already happens.
 */
static inline int pm_generic_suspend(struct device *dev) { (void)dev; return 0; }
static inline int pm_generic_resume(struct device *dev) { (void)dev; return 0; }
static inline int pm_generic_freeze(struct device *dev) { (void)dev; return 0; }
static inline int pm_generic_thaw(struct device *dev) { (void)dev; return 0; }
static inline int pm_generic_poweroff(struct device *dev) { (void)dev; return 0; }
static inline int pm_generic_restore(struct device *dev) { (void)dev; return 0; }
static inline int pm_generic_runtime_suspend(struct device *dev) { (void)dev; return 0; }
static inline int pm_generic_runtime_resume(struct device *dev) { (void)dev; return 0; }

#endif
