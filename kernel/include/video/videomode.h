/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_VIDEO_VIDEOMODE_H
#define LKPI_VIDEO_VIDEOMODE_H
#include <linux/types.h>
/* A display mode in the timing form panel and bridge drivers use — porches and
 * sync widths rather than the total/sync-start pairs DRM's own drm_display_mode
 * carries. The two describe the same signal; the DRM core converts between
 * them, which is why both spellings exist. */
struct videomode {
	unsigned long pixelclock;
	u32 hactive, hfront_porch, hback_porch, hsync_len;
	u32 vactive, vfront_porch, vback_porch, vsync_len;
	u32 flags;
};
#endif
