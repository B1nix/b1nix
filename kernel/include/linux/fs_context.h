/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FS_CONTEXT_H
#define LKPI_LINUX_FS_CONTEXT_H

#include <linux/types.h>
#include <linux/errno.h>

/*
 * The mount context: the new mount API, which ext4 uses and btrfs does not.
 *
 * The difference from the old `->mount(fs_type, flags, dev_name, data)` is that
 * options are parsed one at a time as they arrive, each into a typed value,
 * with errors reported before anything is allocated. The filesystem gets a
 * context to accumulate into (`fs_private`) and is asked to produce a
 * superblock only at `get_tree`.
 *
 * The purpose field is not decoration: `FS_CONTEXT_FOR_RECONFIGURE` is how a
 * remount is distinguished from a mount, and ext4 rejects options on a remount
 * that it accepts on a mount.
 */

struct super_block;
struct dentry;
struct file_system_type;
struct fs_context;
struct path;
struct cred;
struct user_namespace;

enum fs_context_purpose {
	FS_CONTEXT_FOR_MOUNT,
	FS_CONTEXT_FOR_SUBMOUNT,
	FS_CONTEXT_FOR_RECONFIGURE,
};

enum fs_context_phase {
	FS_CONTEXT_CREATE_PARAMS,
	FS_CONTEXT_CREATING,
	FS_CONTEXT_AWAITING_MOUNT,
	FS_CONTEXT_AWAITING_RECONF,
	FS_CONTEXT_RECONF_PARAMS,
	FS_CONTEXT_RECONFIGURING,
	FS_CONTEXT_FAILED,
};

enum fs_value_type {
	fs_value_is_undefined,
	fs_value_is_flag,       /* the option was present with no value */
	fs_value_is_string,
	fs_value_is_blob,
	fs_value_is_filename,
	fs_value_is_file,
};

struct fs_parameter {
	const char *key;
	enum fs_value_type type;
	union {
		char *string;
		void *blob;
		struct filename *name;
		struct file *file;
	};
	size_t size;
	int dirfd;
};

struct fs_context_operations {
	void (*free)(struct fs_context *fc);
	int (*dup)(struct fs_context *fc, struct fs_context *src_fc);
	int (*parse_param)(struct fs_context *fc, struct fs_parameter *param);
	int (*parse_monolithic)(struct fs_context *fc, void *data);
	int (*get_tree)(struct fs_context *fc);
	int (*reconfigure)(struct fs_context *fc);
};

struct fs_context {
	const struct fs_context_operations *ops;
	struct file_system_type *fs_type;
	void *fs_private;      /* the filesystem's accumulated options */
	void *sget_key;
	struct dentry *root;
	struct user_namespace *user_ns;
	const struct cred *cred;
	char *source;          /* the device or source string */
	void *security;
	void *s_fs_info;       /* handed to the superblock at fill time */
	unsigned int sb_flags;
	unsigned int sb_flags_mask;
	unsigned int lsm_flags;
	enum fs_context_purpose purpose : 8;
	enum fs_context_phase phase : 8;
	bool need_free : 1;
	bool global : 1;
	bool oldapi : 1;
};

/*
 * Filling the superblock. `fill_super` is called once, with a superblock that
 * is new and empty, and its return value decides whether the mount happens.
 */
int get_tree_bdev(struct fs_context *fc,
                  int (*fill_super)(struct super_block *sb,
                                    struct fs_context *fc));
int get_tree_nodev(struct fs_context *fc,
                   int (*fill_super)(struct super_block *sb,
                                     struct fs_context *fc));
int get_tree_single(struct fs_context *fc,
                    int (*fill_super)(struct super_block *sb,
                                      struct fs_context *fc));
int get_tree_keyed(struct fs_context *fc,
                   int (*fill_super)(struct super_block *sb,
                                     struct fs_context *fc),
                   void *key);
int get_tree_block_key(struct fs_context *fc,
                       int (*fill_super)(struct super_block *sb,
                                         struct fs_context *fc),
                       void *key);

int vfs_parse_fs_param(struct fs_context *fc, struct fs_parameter *param);
int vfs_parse_fs_string(struct fs_context *fc, const char *key,
                        const char *value, size_t v_size);
int generic_parse_monolithic(struct fs_context *fc, void *data);
struct fs_context *fs_context_for_mount(struct file_system_type *fs_type,
                                        unsigned int sb_flags);
struct fs_context *fs_context_for_reconfigure(struct dentry *dentry,
                                              unsigned int sb_flags,
                                              unsigned int sb_flags_mask);
void put_fs_context(struct fs_context *fc);

/*
 * Reporting a bad option. Upstream logs these into the context so userspace can
 * read back which option was rejected; here they go to the kernel log and
 * return the error, which keeps the RETURN VALUE — the part callers propagate —
 * identical.
 */
int logfc_error(struct fs_context *fc, const char *fmt, ...);

#define infof(fc, fmt, ...)   ((void)(fc))
#define warnf(fc, fmt, ...)   ((void)(fc))
#define errorf(fc, fmt, ...)  logfc_error(fc, fmt, ##__VA_ARGS__)
#define invalf(fc, fmt, ...)  (errorf(fc, fmt, ##__VA_ARGS__), -EINVAL)

#endif
