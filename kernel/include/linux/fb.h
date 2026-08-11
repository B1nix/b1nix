/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_FB_H
#define LKPI_LINUX_FB_H
#include <linux/device.h>
#include <linux/types.h>
/* The legacy framebuffer layer. b1nix has its own /dev/fb (kernel/dev/fb.c);
 * wiring the DRM fbdev emulation onto it is a decision that belongs with the
 * first driver that wants a console, not with the core. Declared to the extent
 * the core names it. */
struct fb_info { void *par; struct device *device; };
struct fb_ops;
struct fb_var_screeninfo { u32 xres, yres, bits_per_pixel; };
struct fb_fix_screeninfo { unsigned long smem_start; u32 smem_len; };

/* Blanking levels, as the panel driver reports them. POWERDOWN is the deepest:
 * the backlight is off and so is the panel's own supply. */
#define FB_BLANK_UNBLANK       0
#define FB_BLANK_NORMAL        1
#define FB_BLANK_VSYNC_SUSPEND 2
#define FB_BLANK_HSYNC_SUSPEND 3
#define FB_BLANK_POWERDOWN     4


/* Whether the framebuffer console is running or parked. b1nix's console is not
 * an fbcon and does not consult this; a driver sets it around a mode change so
 * that fbcon stops touching the framebuffer, and here there is no fbcon to
 * stop. */
#define FBINFO_STATE_RUNNING   0
#define FBINFO_STATE_SUSPENDED 1

#endif
