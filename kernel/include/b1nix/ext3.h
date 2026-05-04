#ifndef B1NIX_EXT3_H
#define B1NIX_EXT3_H

#include <b1nix/types.h>

/* 
 * Ext3 = Ext2 + journaling.
 * We reuse the Ext2 structures (superblock, inode, bgd, etc.)
 * but add journal recovery.
 * 
 * Key differences from Ext2:
 *   - feature_compat HAS_JOURNAL  (0x0004)
 *   - feature_incompat RECOVER    (0x0004) if journal needs replay
 *   - Superblock has s_journal_inum, s_journal_dev
 */

/* Journal superblock is at the start of the journal inode's data blocks */
#define EXT3_JOURNAL_MAGIC     0xC03B3998
#define EXT3_JOURNAL_SUPER_V1  3
#define EXT3_JOURNAL_SUPER_V2  4

struct ext3_journal_superblock {
    u32  js_magic;           /* EXT3_JOURNAL_MAGIC */
    u32  js_expiry;
    u32  js_nblocks;         /* Total journal blocks */
    u32  js_first;           /* First usable log block */
    u32  js_seq;             /* Next transaction sequence number */
    u32  js_maxlen;          /* Max blocks per transaction */
    u32  js_errcode;
    u32  js_feature_compat;
    u32  js_feature_incompat;
    u32  js_feature_ro_compat;
    /* V2 fields */
    u8   js_uuid[16];
    u32  js_nusers;
    u32  js_dynamic;
    u32  js_max_commit;
    u32  js_revoke_maxlen;
    u32  js_revoke_csum_size;
    u32  js_be32_1;
    u32  js_be32_2;
    u32  js_last_fs_blocks[2]; /* FS blocks at last commit */
    u32  js_seq_bitmap[2];
    u8   js_be32_pad[40];
    u32  js_csum_type;
    u32  js_padding[45];
    u32  js_checksum;
} __attribute__((packed));

/* Block types in journal */
#define EXT3_JOURNAL_DESCRIPTOR_BLOCK 1
#define EXT3_JOURNAL_COMMIT_BLOCK     2
#define EXT3_JOURNAL_REVOKE_BLOCK     5

struct ext3_journal_header {
    u32 h_magic;
    u32 h_blocktype;    /* 1 descriptor, 2 commit, 5 revoke */
    u32 h_sequence;     /* Transaction sequence number */
} __attribute__((packed));

struct ext3_journal_block_tag {
    u32 t_blocknr;      /* Block number in the filesystem */
    u32 t_flags;        /* Flags */
} __attribute__((packed));

/* Tag flags */
#define EXT3_JOURNAL_TAG_SAME_UUID  (1 << 0)
#define EXT3_JOURNAL_TAG_LAST_TAG   (1 << 1)
#define EXT3_JOURNAL_TAG_ESC        (1 << 2)

void ext3_init(void);

#endif /* B1NIX_EXT3_H */
