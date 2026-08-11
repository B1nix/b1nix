/* SPDX-License-Identifier: MIT */
#ifndef LKPI_ACPI_VIDEO_H
#define LKPI_ACPI_VIDEO_H
#include <linux/types.h>
/* ACPI backlight and display-switch handoff. b1nix parses ACPI tables but has
 * no interpreter, so there is no _DSM to call: the driver is told no ACPI
 * backlight is present, which is true here, and it keeps its native control. */
enum acpi_backlight_type { acpi_backlight_undef = -1, acpi_backlight_none = 0,
                           acpi_backlight_video, acpi_backlight_vendor,
                           acpi_backlight_native, acpi_backlight_nvidia_wmi_ec };
static inline enum acpi_backlight_type acpi_video_get_backlight_type(void)
{ return acpi_backlight_native; }
static inline void acpi_video_register(void) {}
static inline void acpi_video_unregister(void) {}
static inline int acpi_video_get_edid(void *device, int type, int device_id,
                                      void **edid)
{ (void)device; (void)type; (void)device_id; (void)edid; return -1; }
#endif
