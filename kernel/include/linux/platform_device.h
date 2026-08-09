/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_PLATFORM_DEVICE_H
#define LKPI_LINUX_PLATFORM_DEVICE_H
#include <linux/device.h>
#include <linux/ioport.h>
/* Devices the firmware describes rather than a bus enumerates — an SoC display
 * block. Both M102 targets are PCI functions, so nothing here is instantiated;
 * the types exist for the core's bus-agnostic paths. */
struct platform_device { struct device dev; const char *name; int id; };
struct platform_driver { int (*probe)(struct platform_device *);
                         int (*remove)(struct platform_device *);
                         struct { const char *name; } driver; };
/* Is this device on the platform bus? Nothing here is, since both M102 targets
 * are PCI functions — so the answer is always no and the platform-only paths
 * are never entered. */
#define to_platform_device(d) container_of(d, struct platform_device, dev)

static inline bool dev_is_platform(const struct device *dev)
{
	(void)dev;
	return false;
}

static inline void *platform_get_drvdata(const struct platform_device *pdev)
{ return dev_get_drvdata(&pdev->dev); }
static inline void platform_set_drvdata(struct platform_device *pdev, void *d)
{ dev_set_drvdata(&pdev->dev, d); }
#endif
