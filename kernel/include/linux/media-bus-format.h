/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_MEDIA_BUS_FORMAT_H
#define LKPI_LINUX_MEDIA_BUS_FORMAT_H
/* Pixel formats on a display bus, as the V4L2/DRM bridge layer names them.
 * Only the codes the core mentions; the numbering is ABI and is reproduced
 * exactly rather than renumbered. */
#define MEDIA_BUS_FMT_FIXED           0x0001
#define MEDIA_BUS_FMT_RGB565_1X16     0x1017
#define MEDIA_BUS_FMT_RGB666_1X18     0x1009
#define MEDIA_BUS_FMT_RBG888_1X24     0x100e
#define MEDIA_BUS_FMT_RGB888_1X24     0x100a
#define MEDIA_BUS_FMT_ARGB8888_1X32   0x100d
#define MEDIA_BUS_FMT_UYVY8_1X16      0x200f
#define MEDIA_BUS_FMT_YUV8_1X24       0x2025
#endif
