#ifndef B1NIX_FAT32_H
#define B1NIX_FAT32_H

#include <b1nix/types.h>
#include <b1nix/blk.h>

int fat32_mount(struct block_device *dev, const char *mount_point);

#endif
