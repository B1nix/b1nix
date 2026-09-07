/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MISCDEVICE_H
#define LKPI_LINUX_MISCDEVICE_H

#include <linux/types.h>
#include <linux/errno.h>

/*
 * A character device sharing major 10, distinguished by minor.
 *
 * btrfs registers /dev/btrfs-control this way: it is how `btrfs device scan`
 * tells the kernel about devices before a mount. Registration is what makes the
 * node appear, so this is not a stub to leave empty — but the implementation
 * belongs with b1nix's devfs and is in the lkpi C side, not here.
 */

#define MISC_DYNAMIC_MINOR 255
#define BTRFS_MINOR        234

struct file_operations;

struct miscdevice {
	int minor;
	const char *name;
	const struct file_operations *fops;
	struct list_head list;
	struct device *parent;
	struct device *this_device;
	const char *nodename;
	umode_t mode;
};

int misc_register(struct miscdevice *misc);
void misc_deregister(struct miscdevice *misc);

#endif
