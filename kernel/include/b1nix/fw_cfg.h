/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_FW_CFG_H
#define B1NIX_FW_CFG_H

#include <b1nix/types.h>

#define FW_CFG_PORT_SEL  0x510
#define FW_CFG_PORT_DATA 0x511

#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_FILE_DIR  0x0019

/* Is QEMU's fw_cfg channel there? */
int fw_cfg_present(void);

/* Low-level access functions */
void fw_cfg_select(u16 key);
void fw_cfg_read(void *buf, usize len);

/*
 * Place the graphics OpRegion QEMU offers into memory and point the device at
 * it, which is the guest firmware's job and which SeaBIOS skips for a display
 * that is not of VGA class. Must run before the graphics driver probes.
 * Returns 1 when an OpRegion was placed. See kernel/dev/fw_cfg.c.
 */
int fw_cfg_place_igd_opregion(u8 bus, u8 slot, u8 func);

#endif
