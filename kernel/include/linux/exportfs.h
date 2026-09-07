/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_EXPORTFS_H
#define LKPI_LINUX_EXPORTFS_H

#include <linux/types.h>

/*
 * NFS export support: turning an inode into a handle that survives a server
 * restart, and back again.
 *
 * b1nix has no NFS server, but the structures are not optional — both
 * filesystems fill in an `export_operations` in their superblock
 * unconditionally, and btrfs's `fid` type is used by its own send/receive code
 * to name a subvolume. So the shapes and the handle-type constants are here;
 * nothing calls through the vector.
 *
 * The FILEID_* values are ABI — they go into a file handle a client keeps —
 * and are upstream's.
 */

struct inode;
struct dentry;
struct super_block;

enum fid_type {
	FILEID_ROOT = 0,
	FILEID_INO32_GEN = 1,
	FILEID_INO32_GEN_PARENT = 2,
	FILEID_BTRFS_WITHOUT_PARENT = 0x4d,
	FILEID_BTRFS_WITH_PARENT = 0x4e,
	FILEID_BTRFS_WITH_PARENT_ROOT = 0x4f,
	FILEID_INVALID = 0xff,
};

struct fid {
	union {
		struct {
			u32 ino;
			u32 gen;
			u32 parent_ino;
			u32 parent_gen;
		} i32;
		struct {
			u32 block;
			u16 partref;
			u16 parent_partref;
			u32 generation;
			u32 parent_block;
			u32 parent_generation;
		} udf;
		struct {
			u64 objectid;
			u64 root_objectid;
			u32 gen;
			u64 parent_objectid;
			u32 parent_gen;
			u64 parent_root_objectid;
		} __attribute__((packed)) btrfs;
		__u32 raw[0];
	};
};

struct export_operations {
	int (*encode_fh)(struct inode *inode, __u32 *fh, int *max_len,
	                 struct inode *parent);
	struct dentry *(*fh_to_dentry)(struct super_block *sb, struct fid *fid,
	                               int fh_len, int fh_type);
	struct dentry *(*fh_to_parent)(struct super_block *sb, struct fid *fid,
	                               int fh_len, int fh_type);
	int (*get_name)(struct dentry *parent, char *name, struct dentry *child);
	struct dentry *(*get_parent)(struct dentry *child);
	int (*commit_metadata)(struct inode *inode);
	int (*get_uuid)(struct super_block *sb, u8 *buf, u32 *len, u64 *offset);
	int (*map_blocks)(struct inode *inode, loff_t offset, u64 len,
	                  struct iomap *iomap, bool write, u32 *device_generation);
	int (*commit_blocks)(struct inode *inode, struct iomap *iomaps,
	                     int nr_iomaps, struct iattr *iattr);
	unsigned long flags;
};

#define EXPORT_OP_NOWCC        (0x1)
#define EXPORT_OP_NOSUBTREECHK (0x2)
#define EXPORT_OP_CLOSE_BEFORE_UNLINK (0x4)
#define EXPORT_OP_REMOTE_FS    (0x8)
#define EXPORT_OP_NOATOMIC_ATTR (0x10)
#define EXPORT_OP_FLUSH_ON_CLOSE (0x20)

struct dentry *generic_fh_to_dentry(struct super_block *sb, struct fid *fid,
                                    int fh_len, int fh_type,
                                    struct inode *(*get_inode)(struct super_block *sb,
                                                               u64 ino, u32 gen));
struct dentry *generic_fh_to_parent(struct super_block *sb, struct fid *fid,
                                    int fh_len, int fh_type,
                                    struct inode *(*get_inode)(struct super_block *sb,
                                                               u64 ino, u32 gen));
int generic_encode_ino32_fh(struct inode *inode, __u32 *fh, int *max_len,
                            struct inode *parent);
struct dentry *d_obtain_alias(struct inode *inode);

#endif
