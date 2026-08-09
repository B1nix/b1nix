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
#endif
