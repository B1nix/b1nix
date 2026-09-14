/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BACKING_DEV_H
#define LKPI_LINUX_BACKING_DEV_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/list.h>

/*
 * Backing device info.
 *
 * Upstream this is the writeback control plane: per-device dirty limits, the
 * flusher thread, and the cgroup writeback machinery hanging off it. b1nix has
 * one flusher for the whole block cache and no cgroup writeback, so what is left
 * of a bdi here is what the filesystems actually read out of it — the read-ahead
 * window, the capability bits, and a name for /sys.
 *
 * Keeping it as a real structure rather than deleting the concept matters:
 * btrfs sets `ra_pages` at mount time from its own stripe geometry, and a
 * filesystem whose read-ahead hint goes nowhere reads one block at a time.
 */

struct backing_dev_info {
	unsigned long ra_pages;   /* read-ahead window, in pages */
	unsigned long io_pages;   /* largest single I/O, in pages */
	unsigned int capabilities;
	char name[32];
	atomic_t refcnt;
	/* The driver-model device this bdi belongs to. btrfs links to it from its
	 * own sysfs directory, which is the only use here. */
	struct device *dev;
};

struct device;

/* Capability bits. Only the ones the filesystems test are defined — an
 * unrecognised bit in imported code is a compile error, which is the point. */
#define BDI_CAP_WRITEBACK        (1 << 0)
#define BDI_CAP_WRITEBACK_ACCT   (1 << 1)
#define BDI_CAP_STRICTLIMIT      (1 << 2)

struct inode;
struct super_block;

struct backing_dev_info *inode_to_bdi(struct inode *inode);
/* 1 when the superblock is the block device's own, rather than a filesystem
 * mounted on it. ext4 asks in order to skip work that only makes sense for a
 * real filesystem inode. */
int sb_is_blkdev_sb(struct super_block *sb);

static inline int bdi_write_congested(struct backing_dev_info *bdi)
{
	/* Congestion tracking was removed from Linux in 5.18 and there is nothing
	 * here to track: the block cache queues every request. Never congested. */
	(void)bdi;
	return 0;
}

#endif
