/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_FIRMWARE_H
#define LKPI_LINUX_FIRMWARE_H
#include <lkpi/firmware.h>
#include <linux/device.h>
/* Onto lkpi's request_firmware (M99), which loads a blob through the VFS and
 * reports ENOENT otherwise. Gen8/Gen9.5 needs no firmware; amdgpu does. */
#endif
