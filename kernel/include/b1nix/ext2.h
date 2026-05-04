#ifndef B1NIX_EXT2_H
#define B1NIX_EXT2_H

#include <b1nix/types.h>

#define EXT2_SUPER_MAGIC 0xEF53

struct ext2_superblock {
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
	
	// EXT2_DYNAMIC_REV Specific
	u32 s_first_ino;
	u16 s_inode_size;
	u16 s_block_group_nr;
	u32 s_feature_compat;
	u32 s_feature_incompat;
	u32 s_feature_ro_compat;
	u8  s_uuid[16];
	char s_volume_name[16];
	char s_last_mounted[64];
	u32 s_algo_bitmap;
} __attribute__((packed));

struct ext2_block_group_desc {
	u32 bg_block_bitmap;
	u32 bg_inode_bitmap;
	u32 bg_inode_table;
	u16 bg_free_blocks_count;
	u16 bg_free_inodes_count;
	u16 bg_used_dirs_count;
	u16 bg_pad;
	u8  bg_reserved[12];
} __attribute__((packed));

#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK  (EXT2_IND_BLOCK + 1)
#define EXT2_TIND_BLOCK  (EXT2_DIND_BLOCK + 1)
#define EXT2_N_BLOCKS    (EXT2_TIND_BLOCK + 1)

#define EXT2_S_IFMT  0xF000
#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFLNK 0xA000
#define EXT2_S_IFREG 0x8000
#define EXT2_S_IFBLK 0x6000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFCHR 0x2000
#define EXT2_S_IFIFO 0x1000

struct ext2_inode {
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
	u32 i_block[EXT2_N_BLOCKS];
	u32 i_generation;
	u32 i_file_acl;
	u32 i_dir_acl;
	u32 i_faddr;
	u8  i_osd2[12];
} __attribute__((packed));

#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

struct ext2_dir_entry {
	u32 inode;
	u16 rec_len;
	u8  name_len;
	u8  file_type;
	char name[];
} __attribute__((packed));

void ext2_init(void);

#endif
