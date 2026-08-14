/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BACKLIGHT_H
#define LKPI_LINUX_BACKLIGHT_H
#include <linux/device.h>
/* Panel backlight control. No b1nix subsystem owns brightness yet, so these
 * report absence rather than pretending to dim anything. */
/*
 * A backlight, as the panel driver sees it.
 *
 * b1nix registers none — there is no backlight class here — but i915 keeps its
 * own device and reads props off it while driving the panel natively, so the
 * structure has to be complete. What is absent is the class that would let
 * userspace change brightness, not the driver's own control.
 */
struct backlight_properties {
	int brightness;
	int max_brightness;
	int power;
	int type;
	unsigned int scale;
	unsigned int state;
};

struct backlight_ops;

struct backlight_device {
	struct backlight_properties props;
	const struct backlight_ops *ops;
	void *driver_data;
	struct device dev;
};

struct backlight_ops {
	unsigned int options;
	int (*update_status)(struct backlight_device *);
	int (*get_brightness)(struct backlight_device *);
};

#define BACKLIGHT_RAW       1
#define BACKLIGHT_FIRMWARE  2
#define BACKLIGHT_PLATFORM  3
#define BACKLIGHT_POWER_ON  0
#define BACKLIGHT_POWER_OFF 4
#define BACKLIGHT_SCALE_UNKNOWN 0
#define BL_CORE_SUSPENDRESUME 1

static inline void *bl_get_data(struct backlight_device *bd)
{ return bd ? bd->driver_data : 0; }
static inline struct backlight_device *backlight_device_get_by_name(const char *n)
{ (void)n; return 0; }
static inline int backlight_device_set_brightness(struct backlight_device *bd,
                                                  unsigned long b)
{ (void)bd; (void)b; return -ENODEV; }

/* The blanking levels a backlight's `power` field takes are the framebuffer
 * ones; a panel driver sets them without including <linux/fb.h>. */
#include <linux/fb.h>


/* Turn a backlight on or off through its class device. No class is registered
 * here — see above — so a panel driver that reaches these leaves the backlight
 * wherever its own native control put it. */
static inline int backlight_enable(struct backlight_device *bd)
{ (void)bd; return 0; }
static inline int backlight_disable(struct backlight_device *bd)
{ (void)bd; return 0; }

#endif
