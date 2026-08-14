#ifndef B1NIX_EXT4_H
#define B1NIX_EXT4_H

#include <b1nix/types.h>
#include <b1nix/ext2.h>
#include <b1nix/journal.h>

/*
 * Ext4 features added on top of Ext2/3:
 *
 * 1. EXTENTS (FEATURE_INCOMPAT_EXTENTS 0x0040)
 *    Replaces the indirect block mapping with an extent tree.
 *    Each extent is a (start block, length, physical start) tuple.
 *    Stored in i_block[0..2] (extent tree root header).
 *
 * 2. FLEXIBLE BLOCK GROUPS (FEATURE_INCOMPAT_FLEX_BG 0x0200)
 *    Groups are clustered into "flex groups" for better locality.
 *    Block/inode bitmaps and inode tables are packed together.
 *
 * 3. 64-BIT SUPPORT (FEATURE_INCOMPAT_64BIT 0x0080)
 *    Block group descriptors are 64 bytes instead of 32.
 *    Superblock has *_hi fields for blocks > 16TB.
 *
 * 4. HUGE_FILE (FEATURE_RO_COMPAT_HUGE_FILE 0x0008)
 *    Files > 2TB: i_size_hi, i_blocks_hi, i_blocks is in 512-byte not 512-blocks.
 *
 * 5. EXTRA_ISIZE (FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040)
 *    Inodes can have extra fields beyond 128 bytes (i_extra_isize).
 *
 * 6. LARGEDIR (FEATURE_INCOMPAT_LARGEDIR 0x4000)
 *    Directories with > 2^32 entries use hash tree (HTree).
 *
 * 7. MMP (FEATURE_INCOMPAT_MMP 0x0100)
 *    Multiple mount protection.
 *
 * 8. INLINE_DATA (FEATURE_INCOMPAT_INLINE_DATA 0x8000)
 *    Tiny files stored directly in the inode.
 */

/* ── Extent tree ── */

#define EXT4_EXTENT_MAGIC 0xF30A

struct ext4_extent_header {
    u16 eh_magic;       /* 0xF30A */
    u16 eh_entries;     /* Number of valid entries */
    u16 eh_max;         /* Maximum capacity */
    u16 eh_depth;       /* 0 = leaf, >0 = index nodes */
    u32 eh_generation;
} __attribute__((packed));

struct ext4_extent {
    u32 ee_block;       /* First logical block covered */
    u16 ee_len;         /* Number of blocks (16-bit | 1<<15 = uninitialized) */
    u16 ee_start_hi;
    u32 ee_start_lo;
} __attribute__((packed));

struct ext4_extent_idx {
    u32 ei_block;       /* First logical block in subtree */
    u32 ei_leaf_lo;
    u16 ei_leaf_hi;
    u16 ei_unused;
} __attribute__((packed));

/* ── Ext4 superblock additions ── */

/* Same as ext2_superblock up to s_def_resgid, then extended...
 * We reuse the full superblock from b1nix/ext234.h concept, but
 * for ext4-specific fields we just piggyback on ext2_superblock.
 *
 * The ext2_superblock already has the s_first_ino..s_algo_bitmap fields.
 * For Ext4, we need s_desc_size (u16 at offset 0x106 in sb),
 * s_first_meta_bg, s_blocks_count_hi etc.
 *
 * Instead of a separate struct, we access these via offset calculations
 * from the ext2_superblock pointer cast to a u8 buffer.
 */

/* Offset helpers into the 1024-byte superblock */
#define EXT4_S_DESC_SIZE         0x106  /* u16 */
#define EXT4_S_DEFAULT_MOUNT_OPTS 0x108 /* u32 */
#define EXT4_S_FIRST_META_BG      0x10C /* u32 */
#define EXT4_S_MKFS_TIME          0x110 /* u32 */
#define EXT4_S_BLOCKS_COUNT_HI    0x140 /* u32 */
#define EXT4_S_R_BLOCKS_COUNT_HI  0x144 /* u32 */
#define EXT4_S_FREE_BLOCKS_HI     0x148 /* u32 */
#define EXT4_S_MIN_EXTRA_ISIZE    0x14C /* u16 */
#define EXT4_S_WANT_EXTRA_ISIZE   0x14E /* u16 */
#define EXT4_S_FLAGS              0x150 /* u32 */
#define EXT4_S_LOG_GROUPS_PER_FLEX 0x15E /* u8 */

/* ── Block Group Descriptor 64-bit (Ext4) ── */

struct ext4_bgd_64 {
    u32 bg_block_bitmap_lo;
    u32 bg_inode_bitmap_lo;
    u32 bg_inode_table_lo;
    u16 bg_free_blocks_count_lo;
    u16 bg_free_inodes_count_lo;
    u16 bg_used_dirs_count_lo;
    u16 bg_flags;
    u32 bg_exclude_bitmap_lo;
    u16 bg_block_bitmap_csum_lo;
    u16 bg_inode_bitmap_csum_lo;
    u16 bg_itable_unused_lo;
    u16 bg_checksum;
    u32 bg_block_bitmap_hi;
    u32 bg_inode_bitmap_hi;
    u32 bg_inode_table_hi;
    u16 bg_free_blocks_count_hi;
    u16 bg_free_inodes_count_hi;
    u16 bg_used_dirs_count_hi;
    u16 bg_itable_unused_hi;
    u32 bg_exclude_bitmap_hi;
    u16 bg_block_bitmap_csum_hi;
    u16 bg_inode_bitmap_csum_hi;
    u32 bg_reserved;
    u32 bg_checksum_hi;
} __attribute__((packed));

/* ── Inode flags specific to Ext4 ── */

#define EXT4_EXTENTS_FL     0x00080000  /* Inode uses extents */
#define EXT4_INLINE_DATA_FL 0x10000000  /* Inline data */
#define EXT4_EA_INODE_FL    0x00200000  /* Extended attr inode */
#define EXT4_EOFBLOCKS_FL   0x00400000  /* Blocks allocated beyond EOF */

/* ── Feature incompat flags ── (used with ext2_superblock.s_feature_incompat) */

#define EXT4_FEATURE_INCOMPAT_64BIT    0x0080
#define EXT4_FEATURE_INCOMPAT_EXTENTS  0x0040
#define EXT4_FEATURE_INCOMPAT_FLEX_BG  0x0200
#define EXT4_FEATURE_INCOMPAT_LARGEDIR 0x4000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000
#define EXT4_FEATURE_INCOMPAT_MMP      0x0100

#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE   0x0008
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK   0x0020

struct ext4_fs {
    struct block_device *bdev;
    struct ext2_superblock sb;
    u32 block_size;
    u32 inodes_per_group;
    u32 inode_size;
    u32 desc_size;
    u32 flex_size;
    u32 features_incompat;
    u32 features_ro_compat;
    u32 journal_inum;
    struct ext2_inode journal_inode_cache;
    struct journal_dev *jdev;
    /* Serializes the block/inode allocator bitmaps + superblock counters.
     * Sleeping lock (vfs_meta_lock_*): the holder does block I/O. */
    int alloc_lock;
    /* Where the last allocation stopped. Without it every allocation restarts
     * at group 0, bit 0 and rescans the whole filled prefix, so writing one
     * large file costs O(blocks²) bit tests. Guarded by alloc_lock. */
    u32 alloc_hint_group;
    u32 alloc_hint_bit;
};

struct ext4_inode_info {
    struct ext4_fs *fs;
    u32 inode_num;
};

void ext4_init(void);

#endif /* B1NIX_EXT4_H */
