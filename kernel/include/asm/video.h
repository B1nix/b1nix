/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_VIDEO_H
#define LKPI_ASM_VIDEO_H
#include <linux/types.h>

struct device;

/*
 * Is this the device the firmware initialised as the boot display? Answered
 * by the VGA arbiter upstream, which b1nix does not have; no device is claimed,
 * so sysfs does not advertise boot_vga on any of them.
 */
static inline bool video_is_primary_device(struct device *dev)
{ (void)dev; return false; }

#endif
