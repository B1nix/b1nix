/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CRC16_H
#define LKPI_LINUX_CRC16_H

#include <linux/types.h>

/*
 * CRC-16 with the polynomial 0xA001 (reflected 0x8005) — a third checksum
 * alongside the two in <linux/crc32.h>, and used by ext4 for exactly one thing:
 * the group descriptor checksum on filesystems WITHOUT metadata_csum. Those
 * exist and are mounted; an ext4 image made without that feature checks its
 * group descriptors this way, so getting it wrong makes such a filesystem look
 * corrupt.
 */
u16 crc16(u16 crc, const u8 *buffer, size_t len);

#endif
