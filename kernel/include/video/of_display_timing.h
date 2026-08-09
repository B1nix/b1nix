/* SPDX-License-Identifier: MIT */
#ifndef LKPI_VIDEO_OF_DISPLAY_TIMING_H
#define LKPI_VIDEO_OF_DISPLAY_TIMING_H
#include <linux/of.h>
/* Display timings read from a device tree. x86 has no device tree, so these
 * report absence — see <linux/of.h>. */
struct display_timings;
struct videomode;
static inline int of_get_display_timing(const struct device_node *np,
                                        const char *name, void *dt)
{ (void)np; (void)name; (void)dt; return -EINVAL; }
#endif
