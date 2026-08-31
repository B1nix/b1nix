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

/* One allocated extent, as the extent tree records it: how many references
 * point at it, and what kind of thing it is. Inline backrefs follow the fixed
 * part, and for a non-skinny tree block a btrfs_tree_block_info comes first. */
struct btrfs_extent_item {
    u64 refs;
    u64 generation;
    u64 flags;
} __attribute__((packed));

#define BTRFS_EXTENT_FLAG_DATA       (1ULL << 0)
#define BTRFS_EXTENT_FLAG_TREE_BLOCK (1ULL << 1)

/* The head of an inline backref: a type byte and one u64 whose meaning
 * depends on it. An EXTENT_DATA_REF's u64 is the first field of the
 * btrfs_extent_data_ref that follows instead. */
struct btrfs_extent_inline_ref {
    u8 type;
    u64 offset;
} __attribute__((packed));

/* Who points at a data extent: a file range in one tree. */
struct btrfs_extent_data_ref {
    u64 root;
    u64 objectid;
    u64 offset;
    u32 count;
} __attribute__((packed));

/* An allocated range of the disk, and what may be put in it. */
struct btrfs_block_group_item {
    u64 used;
    u64 chunk_objectid;
    u64 flags;
} __attribute__((packed));

#define BTRFS_BLOCK_GROUP_DATA     (1ULL << 0)
#define BTRFS_BLOCK_GROUP_SYSTEM   (1ULL << 1)
#define BTRFS_BLOCK_GROUP_METADATA (1ULL << 2)
#define BTRFS_BLOCK_GROUP_TYPE_MASK                                            \
    (BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_SYSTEM |                       \
     BTRFS_BLOCK_GROUP_METADATA)

/* Key types, the ones this driver reads and writes. */
#define BTRFS_INODE_ITEM_KEY       1
#define BTRFS_INODE_REF_KEY       12
#define BTRFS_DIR_ITEM_KEY        84
#define BTRFS_DIR_INDEX_KEY       96
#define BTRFS_EXTENT_DATA_KEY    108
#define BTRFS_EXTENT_CSUM_KEY    128
#define BTRFS_ROOT_ITEM_KEY      132
#define BTRFS_EXTENT_ITEM_KEY    168
#define BTRFS_METADATA_ITEM_KEY  169
#define BTRFS_TREE_BLOCK_REF_KEY 176
#define BTRFS_EXTENT_DATA_REF_KEY 178
#define BTRFS_BLOCK_GROUP_ITEM_KEY 192
#define BTRFS_FREE_SPACE_INFO_KEY 198
#define BTRFS_FREE_SPACE_EXTENT_KEY 199
#define BTRFS_FREE_SPACE_BITMAP_KEY 200
#define BTRFS_DEV_EXTENT_KEY     204
#define BTRFS_DEV_ITEM_KEY       216
#define BTRFS_CHUNK_ITEM_KEY     228

/* Well-known objectids. */
#define BTRFS_ROOT_TREE_OBJECTID     1
#define BTRFS_EXTENT_TREE_OBJECTID   2
#define BTRFS_CHUNK_TREE_OBJECTID    3
#define BTRFS_DEV_TREE_OBJECTID      4
#define BTRFS_FS_TREE_OBJECTID       5
#define BTRFS_CSUM_TREE_OBJECTID     7
#define BTRFS_FREE_SPACE_TREE_OBJECTID 10
#define BTRFS_BLOCK_GROUP_TREE_OBJECTID 11
#define BTRFS_DEV_ITEMS_OBJECTID     1
#define BTRFS_FIRST_CHUNK_TREE_OBJECTID 256
#define BTRFS_FIRST_FREE_OBJECTID  256
/* The csum tree keys everything under one negative objectid. */
#define BTRFS_EXTENT_CSUM_OBJECTID ((u64)-10)

/* Feature bits. Only the ones whose on-disk consequences this driver knows how
 * to preserve are accepted for a writable mount; see btrfs_check_rw_features. */
#define BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF   (1ULL << 0)
#define BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL  (1ULL << 1)
#define BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS    (1ULL << 2)
#define BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO    (1ULL << 3)
#define BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD   (1ULL << 4)
#define BTRFS_FEATURE_INCOMPAT_BIG_METADATA    (1ULL << 5)
#define BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF   (1ULL << 6)
#define BTRFS_FEATURE_INCOMPAT_RAID56          (1ULL << 7)
#define BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA (1ULL << 8)
#define BTRFS_FEATURE_INCOMPAT_NO_HOLES        (1ULL << 9)
#define BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE       (1ULL << 0)
#define BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID (1ULL << 1)
#define BTRFS_FEATURE_COMPAT_RO_VERITY                (1ULL << 2)
#define BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE      (1ULL << 3)

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

/* The device, as the chunk tree and the super block both record it. Exactly
 * the 98 bytes the super block reserves for it. */
struct btrfs_dev_item {
    u64 devid;
    u64 total_bytes;
    u64 bytes_used;
    u32 io_align;
    u32 io_width;
    u32 sector_size;
    u64 type;
    u64 generation;
    u64 start_offset;
    u32 dev_group;
    u8 seek_speed;
    u8 bandwidth;
    u8 uuid[16];
    u8 fsid[16];
} __attribute__((packed));

/* Which chunk owns a span of one device. The dev tree holds these, and it is
 * what says a physical range is taken. */
struct btrfs_dev_extent {
    u64 chunk_tree;
    u64 chunk_objectid;
    u64 chunk_offset;
    u64 length;
    u8 chunk_tree_uuid[16];
} __attribute__((packed));

/* The header of one block group's free-space record. */
struct btrfs_free_space_info {
    u32 extent_count;
    u32 flags;
} __attribute__((packed));

#define BTRFS_FREE_SPACE_USING_BITMAPS (1U << 0)

/* One allocated range of the logical address space, from the extent tree. The
 * writable mount keeps the whole set in memory: that is what makes an
 * allocation a question this driver can answer without a free-space tree. */
struct btrfs_alloc_range {
    u64 start;
    u64 len;
};

/* One pending extent-tree update: a reference to add or to drop. */
struct btrfs_delayed_ref {
    u64 bytenr;
    u64 len;
    u64 owner;      /* tree objectid, for a tree block */
    u64 objectid;   /* file, for a data extent */
    u64 file_off;
    u8 level;
    u8 is_data;
    u8 add; /* 1 = insert a reference, 0 = drop one */
};

/* A block group, and where its free space begins. */
struct btrfs_block_group {
    u64 start;
    u64 len;
    u64 flags;
    u64 used;
    /* Set when anything inside this group was allocated or freed, so the
     * commit rebuilds only the free-space records that can have changed. */
    int dirty;
};

#define BTRFS_MAX_BLOCK_GROUPS 64
/* Sized for the filesystems this driver writes to: a test image and a small
 * data disk, whose extent trees hold a few thousand allocations. A mount whose
 * extent tree does not fit stays read-only rather than allocating on top of a
 * map it knows to be incomplete. */
#define BTRFS_MAX_ALLOCS 8192

struct btrfs_fs_info {
    struct block_device *bdev;
    struct btrfs_super_block sb;
    struct btrfs_chunk_map chunks[BTRFS_MAX_CHUNKS];
    u32 nchunks;
    u64 fs_root_bytenr;
    u8 fs_root_level;

    /* Writable-mount state. `rw` is 0 on a read-only mount and nothing below
     * is then populated. */
    int rw;
    u64 extent_root_bytenr;
    u8 extent_root_level;
    u64 csum_root_bytenr;
    u8 csum_root_level;
    /* The free-space tree, when the filesystem has one. Rebuilt from the
     * allocation map at commit; 0 when the feature is absent. */
    u64 fst_root_bytenr;
    u8 fst_root_level;
    int has_fst;
    /* Newer filesystems keep BLOCK_GROUP_ITEMs in a tree of their own rather
     * than in the extent tree. Where that is so, this is its root and every
     * block-group read and write goes here instead. */
    u64 bgt_root_bytenr;
    u8 bgt_root_level;
    int has_bgt;
    /* The chunk tree and the device tree, needed to grow the filesystem by
     * making a new chunk when the existing block groups are full. */
    u64 chunk_root_bytenr;
    u8 chunk_root_level;
    u64 dev_root_bytenr;
    u8 dev_root_level;
    int growing;     /* inside btrfs_alloc_chunk; do not recurse into it */
    int chunk_dirty; /* the chunk tree was written this transaction */
    void *write_hist; /* debug: recent tree-block writes */
    u32 write_hist_n;
    u64 fs_root_dirid;
    /* The generation this mount's writes are stamped with: super.generation+1,
     * fixed for the life of the mount, because every block written under it
     * belongs to the one transaction that the final super-block write
     * commits. */
    u64 trans_gen;
    struct btrfs_block_group bgs[BTRFS_MAX_BLOCK_GROUPS];
    u32 nbgs;
    /* Extent-tree updates waiting to be applied; see the delayed-ref comment
     * in btrfs_write.c. Recording an allocation is itself an allocation, so
     * doing it inline recurses without bound. */
    struct btrfs_delayed_ref *pending;
    u32 npending;
    u32 pending_cap;
    int running_refs;

    struct btrfs_alloc_range *allocs; /* sorted by start */
    u32 nallocs;
    u32 allocs_cap;
    /* Ranges freed during this transaction. They stay out of the allocator
     * until the commit lands: handing one out again while the tree still
     * points at it overwrites live metadata. */
    struct btrfs_alloc_range *pinned;
    u32 npinned;
    u32 pinned_cap;
    int dirty; /* something was written; the super-block owes a commit */
};

struct btrfs_inode_ref {
    u64 index;
    u16 name_len;
} __attribute__((packed));

/* One file's extents, kept on its inode so a read does not walk the tree
 * again. Filled at mount. */
struct btrfs_file_info {
    struct btrfs_fs_info *fs;
    u64 objectid;
    u64 size;
};

int btrfs_mount_root(const char *device_name, const char *mount_point);
void btrfs_init(void);

/* ── Shared with the write path ──────────────────────────────────────────── */

/* crc32c as btrfs checksums use it: Castagnoli, seed ~0, result inverted. */
u32 btrfs_crc32c(const void *data, usize len);
/* The raw form, for the directory name hash: seed ~1, no final inversion. */
u32 btrfs_crc32c_seed(u32 seed, const void *data, usize len);
/* Logical -> physical through the chunk map. 0 when unmapped. */
u64 btrfs_map(struct btrfs_fs_info *fs, u64 logical);
/* One verified tree block; the caller frees it. */
u8 *btrfs_read_block(struct btrfs_fs_info *fs, u64 logical);
int btrfs_key_cmp(const struct btrfs_disk_key *a, u64 objectid, u8 type,
                  u64 offset);
const struct btrfs_item *btrfs_leaf_item(const u8 *leaf, u32 i);
const u8 *btrfs_item_data(const u8 *leaf, const struct btrfs_item *it);

/* ── Write path (kernel/fs/btrfs/btrfs_write.c) ────────────────────────────────── */

/* Bring up the allocator and the write-side roots for a mount that asked to be
 * writable. Returns 0 when the mount may be written to, or a negative errno —
 * the caller then keeps the mount, read-only. */
int btrfs_rw_setup(struct btrfs_fs_info *fs);
void btrfs_rw_teardown(struct btrfs_fs_info *fs);

/* Namespace operations. Each returns 0 or a negative errno, and each leaves
 * the filesystem consistent on failure: an item that could not be inserted
 * takes the ones already written back out. */
int btrfs_create_entry(struct btrfs_fs_info *fs, u64 dir_objectid,
                       const char *name, u32 mode, const char *symlink_target,
                       u64 *objectid_out);
int btrfs_unlink_entry(struct btrfs_fs_info *fs, u64 dir_objectid,
                       const char *name, int is_dir);
int btrfs_link_entry(struct btrfs_fs_info *fs, u64 dir_objectid,
                     const char *name, u64 objectid);
int btrfs_rename_entry(struct btrfs_fs_info *fs, u64 old_dir,
                       const char *old_name, u64 new_dir, const char *new_name);
int btrfs_truncate_file(struct btrfs_fs_info *fs, u64 objectid, u64 length);
int btrfs_setattr_inode(struct btrfs_fs_info *fs, u64 objectid, u32 mode,
                        u32 uid, u32 gid);

/* Replace the contents of one file's byte range. `offset` must be sector
 * aligned; the file grows if the write goes past its end. Returns bytes
 * written or a negative errno. */
isize btrfs_file_write(struct btrfs_fs_info *fs, u64 objectid, u64 offset,
                       const void *buf, usize len, u64 *new_size_out);

/* Push every root the writes moved into the super block, and issue the device
 * flushes that make the transaction durable. */
int btrfs_commit(struct btrfs_fs_info *fs);

/* Mounts a btrfs disk the suite built with mkfs.btrfs and checks what it
 * reads against content derivable from the file's own shape. Emits M119-BTRFS
 * markers; test mode only, and silent when no btrfs disk is attached. */
void btrfs_selftest(void);

#endif
