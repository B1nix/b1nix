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

/* ── On-disk metadata ──────────────────────────────────────────────────────
 *
 * Everything btrfs stores lives in one shape: a B-tree of (objectid, type,
 * offset) keys. A tree block is a header followed either by key pointers to
 * child blocks (level > 0) or by items whose data grows from the END of the
 * block toward the middle (level 0). Reading btrfs is reading that structure;
 * the rest is knowing what each item type holds.
 *
 * Every field is little-endian on disk, which is this kernel's byte order, so
 * the structures are used directly rather than through accessors. */

/* Tree block header. 101 bytes, and the fields after the checksum are what
 * make a block verifiable: a block that is not at the bytenr it claims, or
 * that belongs to a different filesystem, is a stale block the allocator has
 * since reused. */
struct btrfs_header {
    u8 csum[32];
    u8 fsid[16];
    u64 bytenr;
    u64 flags;
    u8 chunk_tree_uuid[16];
    u64 generation;
    u64 owner;
    u32 nritems;
    u8 level;
} __attribute__((packed));

/* Leaf item: where the data sits inside the block, counted back from its end. */
struct btrfs_item {
    struct btrfs_disk_key key;
    u32 offset;
    u32 size;
} __attribute__((packed));

/* Internal-node entry: the child block holding keys >= this one. */
struct btrfs_key_ptr {
    struct btrfs_disk_key key;
    u64 blockptr;
    u64 generation;
} __attribute__((packed));

struct btrfs_timespec {
    u64 sec;
    u32 nsec;
} __attribute__((packed));

struct btrfs_inode_item {
    u64 generation;
    u64 transid;
    u64 size;
    u64 nbytes;
    u64 block_group;
    u32 nlink;
    u32 uid;
    u32 gid;
    u32 mode;
    u64 rdev;
    u64 flags;
    u64 sequence;
    u64 reserved[4];
    struct btrfs_timespec atime;
    struct btrfs_timespec ctime;
    struct btrfs_timespec mtime;
    struct btrfs_timespec otime;
} __attribute__((packed));

/* A name in a directory, and the key of what it names. The name follows. */
struct btrfs_dir_item {
    struct btrfs_disk_key location;
    u64 transid;
    u16 data_len;
    u16 name_len;
    u8 type;
} __attribute__((packed));

/* One extent of a file. `type` decides which half of the union is meaningful:
 * an inline extent carries its bytes right here, a regular one points at disk
 * and may be a slice of a larger allocation (offset/num_bytes within
 * disk_num_bytes). */
struct btrfs_file_extent_item {
    u64 generation;
    u64 ram_bytes;
    u8 compression;
    u8 encryption;
    u16 other_encoding;
    u8 type;
    /* Regular and prealloc extents only; an inline extent's data starts here. */
    u64 disk_bytenr;
    u64 disk_num_bytes;
    u64 offset;
    u64 num_bytes;
} __attribute__((packed));

#define BTRFS_FILE_EXTENT_INLINE  0
#define BTRFS_FILE_EXTENT_REG     1
#define BTRFS_FILE_EXTENT_PREALLOC 2
/* The fixed part of an inline extent, before its bytes. */
#define BTRFS_FILE_EXTENT_INLINE_HDR 21

/* The root of one tree, as the root tree records it. */
struct btrfs_root_item {
    struct btrfs_inode_item inode;
    u64 generation;
    u64 root_dirid;
    u64 bytenr;
    u64 byte_limit;
    u64 bytes_used;
    u64 last_snapshot;
    u64 flags;
    u32 refs;
    struct btrfs_disk_key drop_progress;
    u8 drop_level;
    u8 level;
} __attribute__((packed));

/* Key types, the ones this driver reads. */
#define BTRFS_INODE_ITEM_KEY       1
#define BTRFS_INODE_REF_KEY       12
#define BTRFS_DIR_ITEM_KEY        84
#define BTRFS_DIR_INDEX_KEY       96
#define BTRFS_EXTENT_DATA_KEY    108
#define BTRFS_ROOT_ITEM_KEY      132
#define BTRFS_CHUNK_ITEM_KEY     228

/* Well-known objectids. */
#define BTRFS_ROOT_TREE_OBJECTID     1
#define BTRFS_FS_TREE_OBJECTID       5
#define BTRFS_FIRST_FREE_OBJECTID  256

/* DIR_ITEM types, which mirror the POSIX file types. */
#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR      2
#define BTRFS_FT_SYMLINK  7

/* One logical->physical mapping, read from the chunk tree. A chunk maps a
 * range of the filesystem's logical address space onto a device. Only
 * single-device, single-stripe profiles are mapped here; anything else is
 * refused at mount rather than read through the first stripe and hoped for. */
struct btrfs_chunk_map {
    u64 logical;
    u64 length;
    u64 physical;
};

#define BTRFS_MAX_CHUNKS 256

struct btrfs_fs_info {
    struct block_device *bdev;
    struct btrfs_super_block sb;
    struct btrfs_chunk_map chunks[BTRFS_MAX_CHUNKS];
    u32 nchunks;
    u64 fs_root_bytenr;
    u8 fs_root_level;
};

/* One file's extents, kept on its inode so a read does not walk the tree
 * again. Filled at mount. */
struct btrfs_file_info {
    struct btrfs_fs_info *fs;
    u64 objectid;
    u64 size;
};

int btrfs_mount_root(const char *device_name, const char *mount_point);
void btrfs_init(void);

/* Mounts a btrfs disk the suite built with mkfs.btrfs and checks what it
 * reads against content derivable from the file's own shape. Emits M119-BTRFS
 * markers; test mode only, and silent when no btrfs disk is attached. */
void btrfs_selftest(void);

#endif
