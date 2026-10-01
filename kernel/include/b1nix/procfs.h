/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_PROCFS_H
#define B1NIX_PROCFS_H

/* Register the synthetic /proc (procfs) and /sys (sysfs) filesystems. Call
 * before mounting them in vfs_init/kernel_main. */
void procfs_init(void);
void sysfs_init(void);
/* Publish the CPUs that came up after /sys was mounted. */
void sysfs_cpus_online(void);

#endif
