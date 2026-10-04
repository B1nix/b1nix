/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CDROM_H
#define LKPI_LINUX_CDROM_H

#include <linux/blkdev.h>
/* The ioctl structures (struct cdrom_tocentry, struct cdrom_multisession,
 * CDROM_LBA, ...) are imported, not restated: they are an ABI. */
#include <uapi/linux/cdrom.h>

/*
 * The CD-ROM layer, as far as a filesystem sees it.
 *
 * isofs asks a CD drive one thing: where the last session of a multisession
 * disc starts, so it reads the newest volume descriptors rather than the
 * first. b1nix has no Uniform CD-ROM driver behind its block devices, so no
 * disk has a cdrom_device_info and disk_to_cdi() says so, exactly as upstream
 * does on a kernel built without CONFIG_CDROM. isofs then reads the volume
 * descriptors at the start of the medium, which is where a single-session
 * disc and every ISO image file keep them.
 */

struct cdrom_device_info;

static inline struct cdrom_device_info *disk_to_cdi(struct gendisk *disk)
{
	(void)disk;
	return NULL;
}

static inline int cdrom_read_tocentry(struct cdrom_device_info *cdi,
                                      struct cdrom_tocentry *entry)
{
	(void)cdi;
	(void)entry;
	return -ENOSYS;
}

static inline int cdrom_multisession(struct cdrom_device_info *cdi,
                                     struct cdrom_multisession *info)
{
	(void)cdi;
	(void)info;
	return -ENOSYS;
}

#endif
