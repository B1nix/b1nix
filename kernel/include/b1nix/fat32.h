#ifndef B1NIX_FAT32_H
#define B1NIX_FAT32_H

#include <b1nix/types.h>
#include <b1nix/blk.h>

struct fat32_fs {
	struct block_device *bdev;
	u32 block_size;
	u32 sectors_per_cluster;
	u32 data_start_sector;
	u32 fat_start_sector;
	u32 root_cluster;
	u32 total_clusters;
	u32 free_clusters;
	u32 sectors_per_fat;
    u16 bytes_per_sector;
    u16 fsinfo_sector;
    u8 fat_dirty;
    u8 fsinfo_dirty;
    struct fat32_fs *next;
};

struct fat32_inode_info {
    struct fat32_fs *fs;
    u32 first_cluster;
    usize size;
    u32 entry_sector;
    u32 entry_offset;
};

int fat32_mount(struct block_device *dev, const char *mount_point);
void fat32_init(void);
void fat32_sync_all_fs(void);

#endif
