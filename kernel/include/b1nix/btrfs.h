#ifndef B1NIX_BTRFS_H
#define B1NIX_BTRFS_H

void btrfs_init(void);
int btrfs_mount_root(const char *device_name, const char *mount_point);

#endif
