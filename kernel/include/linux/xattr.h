/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_XATTR_H
#define LKPI_LINUX_XATTR_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <uapi/linux/xattr.h>

/*
 * Extended attributes.
 *
 * The dispatch is by name prefix: a handler claims `user.`, `trusted.`,
 * `security.` or `system.`, and the VFS picks one by matching the front of the
 * name. Both filesystems register handler arrays and the array is terminated by
 * a NULL entry, so the shape matters as much as the functions.
 *
 * `xattr_full_name` is the subtlety worth writing down: a handler is called
 * with the name *after* its prefix, and reconstructing the full name means
 * stepping back over the prefix the handler itself declares. Getting it wrong
 * stores attributes under a truncated name that nothing can read back.
 */

struct inode;
struct dentry;
struct mnt_idmap;
struct super_block;

struct xattr_handler {
	const char *name;
	const char *prefix;
	int flags;   /* the filesystem's own private tag for this handler */
	bool (*list)(struct dentry *dentry);
	int (*get)(const struct xattr_handler *handler, struct dentry *dentry,
	           struct inode *inode, const char *name, void *buffer,
	           size_t size);
	int (*set)(const struct xattr_handler *handler, struct mnt_idmap *idmap,
	           struct dentry *dentry, struct inode *inode, const char *name,
	           const void *buffer, size_t size, int flags);
};

struct xattr {
	const char *name;
	void *value;
	size_t value_len;
};

static inline const char *xattr_prefix(const struct xattr_handler *handler)
{
	return handler->prefix ? handler->prefix : handler->name;
}

static inline const char *xattr_full_name(const struct xattr_handler *handler,
                                          const char *name)
{
	size_t prefix_len = strlen(xattr_prefix(handler));

	return name - prefix_len;
}

static inline bool xattr_handler_can_list(const struct xattr_handler *handler,
                                          struct dentry *dentry)
{
	return handler && (!handler->list || handler->list(dentry));
}

ssize_t generic_listxattr(struct dentry *dentry, char *buffer, size_t size);
ssize_t vfs_getxattr(struct mnt_idmap *idmap, struct dentry *dentry,
                     const char *name, void *value, size_t size);
int vfs_setxattr(struct mnt_idmap *idmap, struct dentry *dentry,
                 const char *name, const void *value, size_t size, int flags);
int __vfs_setxattr(struct mnt_idmap *idmap, struct dentry *dentry,
                   struct inode *inode, const char *name, const void *value,
                   size_t size, int flags);
ssize_t __vfs_getxattr(struct dentry *dentry, struct inode *inode,
                       const char *name, void *value, size_t size);
int vfs_removexattr(struct mnt_idmap *idmap, struct dentry *dentry,
                    const char *name);

const char *xattr_full_name(const struct xattr_handler *, const char *);

/* The pointer is const-qualified upstream; the qualifier on a returned
 * pointer VALUE has no effect and clang says so, so it is dropped here. */
extern const struct xattr_handler *xattr_resolve_name(struct inode *inode,
                                                     const char **name);

#endif
