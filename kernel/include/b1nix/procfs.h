#ifndef B1NIX_PROCFS_H
#define B1NIX_PROCFS_H

/* Register the synthetic /proc (procfs) and /sys (sysfs) filesystems. Call
 * before mounting them in vfs_init/kernel_main. */
void procfs_init(void);
void sysfs_init(void);

#endif
