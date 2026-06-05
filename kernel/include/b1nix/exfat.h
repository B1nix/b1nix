#ifndef B1NIX_EXFAT_H
#define B1NIX_EXFAT_H

#include <b1nix/types.h>
#include <b1nix/blk.h>
#include <b1nix/vfs.h>

struct exfat_boot_sector {
    u8 jmp[3];
    char fs_name[8];
    u8 zero[53];
    u64 partition_offset;
    u64 volume_length;
    u32 fat_offset;
    u32 fat_length;
    u32 cluster_heap_offset;
    u32 cluster_count;
    u32 root_dir_cluster;
    u32 volume_serial_number;
    u16 fs_revision;
    u16 volume_flags;
    u8 bytes_per_sector_shift;
    u8 sectors_per_cluster_shift;
    u8 number_of_fats;
    u8 drive_select;
    u8 percent_in_use;
    u8 reserved[7];
    u8 boot_code[390];
    u16 signature;
} __attribute__((packed));

struct exfat_fs {
    struct block_device *bdev;
    u64 partition_offset;
    u64 volume_length;
    u32 fat_offset;
    u32 fat_length;
    u32 cluster_heap_offset;
    u32 cluster_count;
    u32 root_dir_cluster;
    u8 bytes_per_sector_shift;
    u8 sectors_per_cluster_shift;
    u8 number_of_fats;
    struct exfat_fs *next;
};

struct exfat_inode_info {
    struct exfat_fs *fs;
    u32 first_cluster;
    usize size;
    u8 no_fat_chain;
    u8 is_dir;
};

struct exfat_dir_entry {
    u8 type;
    u8 data[31];
} __attribute__((packed));

struct exfat_entry_file {
    u8 type;
    u8 secondary_count;
    u16 set_checksum;
    u16 file_attributes;
    u16 reserved1;
    u32 create_timestamp;
    u32 last_modified_timestamp;
    u32 last_accessed_timestamp;
    u8 create_10ms_increment;
    u8 last_modified_10ms_increment;
    u8 create_utc_offset;
    u8 last_modified_utc_offset;
    u8 last_accessed_utc_offset;
    u8 reserved2[7];
} __attribute__((packed));

struct exfat_entry_stream {
    u8 type;
    u8 general_secondary_flags;
    u8 reserved1;
    u8 name_length;
    u16 name_hash;
    u16 reserved2;
    u64 valid_data_length;
    u32 reserved3;
    u32 first_cluster;
    u64 data_length;
} __attribute__((packed));

struct exfat_entry_name {
    u8 type;
    u8 general_secondary_flags;
    u16 file_name[15];
} __attribute__((packed));

void exfat_init(void);

#endif
