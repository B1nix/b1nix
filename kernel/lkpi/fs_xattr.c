/* SPDX-License-Identifier: MIT */
/*
 * Extended attributes: the dispatch between a name and the handler that owns
 * its prefix.
 *
 * Both imported filesystems implement xattrs the same way: the superblock
 * carries a NULL-terminated array of `struct xattr_handler`, each claiming a
 * prefix ("user.", "trusted.", "security.", "system."), and the handler is
 * called with the name AFTER that prefix. Listing goes the other way, through
 * the inode's own ->listxattr, because only the filesystem knows what it
 * stores.
 *
 * Nothing above this file existed: the handlers compiled, registered and were
 * unreachable, so an attribute written through b1nix went to the in-memory
 * list its VFS keeps and never reached the disk.
 */

#include <linux/fs.h>
#include <linux/xattr.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mount.h>

/*
 * Find the handler for `*name` and advance the name past its prefix.
 *
 * The empty prefix is upstream's convention for a handler that claims every
 * name it is offered, and it must be tried LAST — otherwise it swallows the
 * names the prefixed handlers own.
 */
const struct xattr_handler *xattr_resolve_name(struct inode *inode,
                                               const char **name)
{
	const struct xattr_handler *const *handlers;
	const struct xattr_handler *const *h;

	if (!inode || !inode->i_sb || !inode->i_sb->s_xattr)
		return ERR_PTR(-EOPNOTSUPP);
	handlers = inode->i_sb->s_xattr;

	if (**name == '\0')
		return ERR_PTR(-EINVAL);

	for (h = handlers; *h; h++) {
		const char *prefix = xattr_prefix(*h);
		size_t len;

		if (!prefix || !*prefix)
			continue;
		len = strlen(prefix);
		if (strncmp(*name, prefix, len) == 0) {
			if ((*name)[len] == '\0')
				return ERR_PTR(-EINVAL);
			*name += len;
			return *h;
		}
	}
	for (h = handlers; *h; h++) {
		const char *prefix = xattr_prefix(*h);

		if (!prefix || !*prefix)
			return *h;
	}
	return ERR_PTR(-EOPNOTSUPP);
}

ssize_t __vfs_getxattr(struct dentry *dentry, struct inode *inode,
                       const char *name, void *value, size_t size)
{
	const struct xattr_handler *handler;

	handler = xattr_resolve_name(inode, &name);
	if (IS_ERR(handler))
		return PTR_ERR(handler);
	if (!handler->get)
		return -EOPNOTSUPP;
	return handler->get(handler, dentry, inode, name, value, size);
}

int __vfs_setxattr(struct mnt_idmap *idmap, struct dentry *dentry,
                   struct inode *inode, const char *name, const void *value,
                   size_t size, int flags)
{
	const struct xattr_handler *handler;

	handler = xattr_resolve_name(inode, &name);
	if (IS_ERR(handler))
		return (int)PTR_ERR(handler);
	if (!handler->set)
		return -EOPNOTSUPP;
	return handler->set(handler, idmap, dentry, inode, name, value, size,
	                    flags);
}

/*
 * The generic listing: every handler that claims to be listable, by prefix.
 * Neither imported filesystem uses it — both keep their own list of what they
 * actually stored — but it is what a filesystem without its own ->listxattr
 * gets, and leaving it undefined would be a link error the day one appears.
 */
ssize_t generic_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	const struct xattr_handler *const *h;
	ssize_t used = 0;

	if (!dentry || !dentry->d_inode || !dentry->d_inode->i_sb->s_xattr)
		return -EOPNOTSUPP;

	for (h = dentry->d_inode->i_sb->s_xattr; *h; h++) {
		const char *prefix = xattr_prefix(*h);
		size_t len;

		if (!prefix || !*prefix || !xattr_handler_can_list(*h, dentry))
			continue;
		len = strlen(prefix) + 1;
		if (buffer) {
			if ((size_t)used + len > size)
				return -ERANGE;
			memcpy(buffer + used, prefix, len);
		}
		used += (ssize_t)len;
	}
	return used;
}
