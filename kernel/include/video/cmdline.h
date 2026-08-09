/* SPDX-License-Identifier: MIT */
#ifndef LKPI_VIDEO_CMDLINE_H
#define LKPI_VIDEO_CMDLINE_H
#include <linux/types.h>
/* Per-connector mode overrides from the kernel command line (video=HDMI-A-1:...).
 * b1nix parses its own cmdline but exposes nothing here yet, so every lookup
 * reports absence and the driver picks the mode itself. */
static inline const char *video_get_options(const char *name)
{ (void)name; return 0; }
#endif
