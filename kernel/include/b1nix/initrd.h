/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_INITRD_H
#define B1NIX_INITRD_H

/* Is the boot loader's ramdisk an initramfs (a cpio archive, possibly
 * compressed) rather than a filesystem image? `noinitrd` says no to both. */
int initrd_is_initramfs(void);

/* Mount a tmpfs at "/" and unpack the initramfs into it. 0 on success. */
int initrd_unpack_to_rootfs(void);

#endif
