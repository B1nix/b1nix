/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_RAMDISK_H
#define B1NIX_RAMDISK_H

void ramdisk_init(void);
/* The boot module was an initramfs and has been unpacked: stop publishing it
 * as ram0. */
void ramdisk_release(void);

/* Print ram0's address and the first bytes of its ext4 superblock. Repeatable:
 * the boot-time copy has scrolled off long before a mount failure is noticed. */
void ramdisk_report(void);

#endif
