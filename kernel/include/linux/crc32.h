/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CRC32_H
#define LKPI_LINUX_CRC32_H

#include <linux/types.h>
#include <linux/crc32c.h>

/*
 * CRC-32 (the Ethernet/zlib polynomial), which is a different polynomial from
 * CRC-32C and produces different values. ext4 uses this one for its directory
 * index hashes and the little-endian form for its own metadata.
 */
u32 crc32_le(u32 crc, const unsigned char *p, size_t len);
u32 crc32_be(u32 crc, const unsigned char *p, size_t len);
u32 __crc32c_le(u32 crc, const unsigned char *p, size_t len);

#define crc32(seed, data, length) crc32_le(seed, (const unsigned char *)(data), length)

#endif
