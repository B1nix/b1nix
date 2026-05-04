#ifndef B1NIX_EXT1_H
#define B1NIX_EXT1_H

#include <b1nix/types.h>

#define EXT1_SUPER_MAGIC 0xEF53

struct ext1_superblock {
    u32 s_inodes_count;
    u32 s_blocks_count;
    u32 s_r_blocks_count;
    u32 s_free_blocks_count;
    u32 s_free_inodes_count;
    u32 s_first_data_block;
    u32 s_log_block_size;
    u32 s_log_frag_size;
    u32 s_blocks_per_group;
    u32 s_frags_per_group;
    u32 s_inodes_per_group;
    u32 s_mtime;
    u32 s_wtime;
    u16 s_mnt_count;
    u16 s_max_mnt_count;
    u16 s_magic;
    u16 s_state;
    u16 s_errors;
    u16 s_minor_rev_level;
    u32 s_lastcheck;
    u32 s_checkinterval;
    u32 s_creator_os;
    u32 s_rev_level;
    u16 s_def_resuid;
    u16 s_def_resgid;
} __attribute__((packed));

struct ext1_inode {
    u16 i_mode;
    u16 i_uid;
    u32 i_size;
    u32 i_atime;
    u32 i_ctime;
    u32 i_mtime;
    u32 i_dtime;
    u16 i_gid;
    u16 i_links_count;
    u32 i_blocks;
    u32 i_flags;
    u32 i_osd1;
    u32 i_block[15];
} __attribute__((packed));

#define EXT1_NDIR_BLOCKS 12
#define EXT1_IND_BLOCK   12
#define EXT1_DIND_BLOCK  13
#define EXT1_TIND_BLOCK  14

struct ext1_dir_entry {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    char name[];
} __attribute__((packed));

/* Ext1 block group descriptor (same layout as Ext2) */
struct ext1_bgd {
    u32 bg_block_bitmap;
    u32 bg_inode_bitmap;
    u32 bg_inode_table;
    u16 bg_free_blocks_count;
    u16 bg_free_inodes_count;
    u16 bg_used_dirs_count;
    u16 bg_pad;
    u8  bg_reserved[12];
} __attribute__((packed));

/* Mode bits */
#define EXT1_S_IFMT   0xF000
#define EXT1_S_IFSOCK 0xC000
#define EXT1_S_IFLNK  0xA000
#define EXT1_S_IFREG  0x8000
#define EXT1_S_IFBLK  0x6000
#define EXT1_S_IFDIR  0x4000
#define EXT1_S_IFCHR  0x2000
#define EXT1_S_IFIFO  0x1000

void ext1_init(void);

#endif
