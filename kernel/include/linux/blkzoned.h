/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BLKZONED_H
#define LKPI_LINUX_BLKZONED_H

#include <linux/types.h>

/*
 * Zoned block devices: the report structure.
 *
 * b1nix has no zoned device and `bdev_is_zoned` always says so, which is what
 * keeps btrfs's zoned paths unreachable. The structure still has to be complete
 * — btrfs declares arrays of it on the stack in code that is compiled either
 * way, and an incomplete type is a compile error there rather than a dead path.
 *
 * The values are upstream's because they are the SCSI/NVMe report format, not
 * an invention: a real zoned device fills these in.
 */

enum blk_zone_type {
	BLK_ZONE_TYPE_CONVENTIONAL = 0x1,
	BLK_ZONE_TYPE_SEQWRITE_REQ = 0x2,
	BLK_ZONE_TYPE_SEQWRITE_PREF = 0x3,
};

enum blk_zone_cond {
	BLK_ZONE_COND_NOT_WP = 0x0,
	BLK_ZONE_COND_EMPTY = 0x1,
	BLK_ZONE_COND_IMP_OPEN = 0x2,
	BLK_ZONE_COND_EXP_OPEN = 0x3,
	BLK_ZONE_COND_CLOSED = 0x4,
	BLK_ZONE_COND_READONLY = 0xD,
	BLK_ZONE_COND_FULL = 0xE,
	BLK_ZONE_COND_OFFLINE = 0xF,
};

struct blk_zone {
	__u64 start;
	__u64 len;
	__u64 wp;        /* the write pointer: where the next write must land */
	__u8 type;
	__u8 cond;
	__u8 non_seq;
	__u8 reset;
	__u8 resv[4];
	__u64 capacity;
	__u8 reserved[24];
};

#endif
