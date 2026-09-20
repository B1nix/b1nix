/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_UFS_H
#define B1NIX_UFS_H

/* UFS host controllers: PCI (QEMU) and the Qualcomm SoC node (b1nix.ufs).
 * Registers every logical unit as an sd* disk. See kernel/dev/ufs.c. */
void ufs_init(void);
/* b1nix.test=1: checks against the smoke suite's UFS disk. */
void ufs_selftest(void);

#endif
