#ifndef B1NIX_BTRFS_H
#define B1NIX_BTRFS_H

#include <b1nix/types.h>
#include <b1nix/blk.h>

#define BTRFS_MAGIC "_BHRfS_M"
#define BTRFS_SUPER_INFO_OFFSET 65536 // 64KB

struct btrfs_disk_key {
    u64 objectid;
    u8 type;
    u64 offset;
} __attribute__((packed));

struct btrfs_stripe {
    u64 devid;
    u64 physical;
    u8 dev_uuid[16];
} __attribute__((packed));

struct btrfs_chunk {
    u64 length;
    u64 owner;
    u64 stripe_len;
    u64 type;
    u32 io_align;
    u32 io_width;
    u32 sector_size;
    u16 num_stripes;
    u16 sub_stripes;
    struct btrfs_stripe stripes[];
} __attribute__((packed));

struct btrfs_super_block {
    u8 csum[32];
    u8 fsid[16];
    u64 bytenr; // Physical address of this block
    u64 flags;
    char magic[8];
    u64 generation;
    u64 root;   // Logical address of root tree
    u64 chunk_root; // Logical address of chunk tree
    u64 log_root;
    u64 log_root_transid;
    u64 total_bytes;
    u64 bytes_used;
    u64 root_dir_objectid;
    u64 num_devices;
    u32 sectorsize;
    u32 nodesize;
    u32 leafsize;
    u32 stripesize;
    u32 sys_chunk_array_size;
    u64 chunk_root_generation;
    u64 compat_flags;
    u64 compat_ro_flags;
    u64 incompat_flags;
    u16 csum_type;
    u8 root_level;
    u8 chunk_root_level;
    u8 log_root_level;
    u8 dev_item[98];
    char label[256];
    u8 unused[256];
    u8 sys_chunk_array[2048];
} __attribute__((packed));

struct btrfs_fs_info {
    struct block_device *bdev;
    struct btrfs_super_block sb;
};

int btrfs_mount_root(const char *device_name, const char *mount_point);
void btrfs_init(void);

#endif
