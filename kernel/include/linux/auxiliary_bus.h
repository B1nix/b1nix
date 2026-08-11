/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_AUXILIARY_BUS_H
#define LKPI_LINUX_AUXILIARY_BUS_H

#include <linux/device.h>
/*
 * The auxiliary bus: how a driver splits a piece of its own hardware off as a
 * separate device for another driver to bind. i915 uses it to hand the GSC
 * (the security controller embedded in the GPU) to the mei driver.
 *
 * b1nix has no mei driver, so nothing would ever bind. Rather than register a
 * device that sits unclaimed, registration is declared and not defined: the
 * object that would create it fails to link, which is the honest statement that
 * this half of the hardware is not driven here.
 */
struct auxiliary_device {
	struct device dev;
	const char *name;
	u32 id;
};
struct auxiliary_driver {
	int (*probe)(struct auxiliary_device *auxdev, const void *id);
	void (*remove)(struct auxiliary_device *auxdev);
	struct device_driver driver;
};
#define to_auxiliary_dev(d) container_of(d, struct auxiliary_device, dev)

int auxiliary_device_init(struct auxiliary_device *auxdev);
int auxiliary_device_add(struct auxiliary_device *auxdev);
void auxiliary_device_uninit(struct auxiliary_device *auxdev);
void auxiliary_device_delete(struct auxiliary_device *auxdev);

#endif
