/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_BACKLIGHT_H
#define LKPI_LINUX_BACKLIGHT_H
#include <linux/device.h>
/* Panel backlight control. No b1nix subsystem owns brightness yet, so these
 * report absence rather than pretending to dim anything. */
struct backlight_device;
struct backlight_properties { int brightness; int max_brightness; int power; };
static inline struct backlight_device *backlight_device_get_by_name(const char *n)
{ (void)n; return 0; }
static inline int backlight_device_set_brightness(struct backlight_device *bd,
                                                  unsigned long b)
{ (void)bd; (void)b; return -ENODEV; }
#endif
