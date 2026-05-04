#ifndef B1NIX_JOURNAL_H
#define B1NIX_JOURNAL_H

#include <b1nix/blk.h>
#include <b1nix/types.h>

/*
 * Simple journaling layer for VFS operations.
 * Uses a write-ahead log (WAL) to ensure atomicity of filesystem operations.
 * 
 * Journal format:
 *   [superblock] [descriptor blocks] [data blocks] [commit block]
 *   
 * Each transaction:
 *   1. Write descriptor (lists blocks being modified)
 *   2. Write data blocks
 *   3. Write commit block
 *   4. Checkpoint: write data to actual locations
 *   5. Replay on mount if commit found without checkpoint
 */

#define JOURNAL_SUPER_MAGIC 0x4A524E4C  // "JRNL"
#define JOURNAL_BLOCK_SIZE  512
#define MAX_JOURNAL_BLOCKS  1024

struct journal_superblock {
    u32 magic;
    u32 block_size;
    u32 total_blocks;
    u32 first_transaction_block;
    u32 transaction_count;
    u32 last_checkpoint;
    u32 flags;
    u8  rsv[500];
} __attribute__((packed));

struct journal_transaction {
    u32 magic;
    u32 block_count;
    u32 checksum;
    u8  rsv[20];
} __attribute__((packed));

#define JOURNAL_DESCRIPTOR_BLOCK 0x4A44454C  // "JDEL"
#define JOURNAL_COMMIT_BLOCK     0x4A434D54  // "JCMT"

struct journal_descriptor {
    u32 magic;        // 0x4A44454C ("JDEL")
    u32 block_count;
    u32 blocks[];     // Array of original block addresses to modify
} __attribute__((packed));

struct journal_commit {
    u32 magic;        // 0x4A434D54 ("JCMT")
    u32 transaction_id;
} __attribute__((packed));

// Journal handle for ongoing transactions
struct journal_handle {
    struct block_device *dev;
    u64 journal_start_lba;
    u32 journal_blocks;
    u32 transaction_id;
    int transaction_active;
    u32 *modified_blocks;
    u32 modified_count;
    u32 max_modified;
};

// Journal API
void journal_init(struct block_device *dev, u64 start_lba, u32 total_blocks);
struct journal_handle *journal_start_transaction(void);
int journal_log_block(struct journal_handle *handle, u64 block_lba, const void *data);
int journal_commit_transaction(struct journal_handle *handle);
void journal_replay(void);
int journal_is_present(void);

#endif
