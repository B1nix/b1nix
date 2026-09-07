/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_NAMEI_H
#define LKPI_LINUX_NAMEI_H

#include <linux/types.h>
#include <linux/path.h>
#include <linux/fs.h>

/*
 * Path resolution, as a filesystem calls into it.
 *
 * Both filesystems reach for it in exactly two situations: resolving a device
 * path given as a mount option (`kern_path`), and looking up a single component
 * in a directory they already hold (`lookup_one_len`). Neither walks a path on
 * behalf of userspace — that is the VFS's job, above them.
 *
 * `LOOKUP_FOLLOW` is the one flag they pass, and it means what it says: resolve
 * a trailing symlink. A `kern_path` that ignored it would hand back the link
 * rather than the device.
 */

#define LOOKUP_FOLLOW      0x0001
#define LOOKUP_DIRECTORY   0x0002
#define LOOKUP_AUTOMOUNT   0x0004
#define LOOKUP_EMPTY       0x4000
#define LOOKUP_DOWN        0x8000
#define LOOKUP_MOUNTPOINT  0x0080
#define LOOKUP_REVAL       0x0020
#define LOOKUP_RCU         0x0040
#define LOOKUP_OPEN        0x0100
#define LOOKUP_CREATE      0x0200
#define LOOKUP_EXCL        0x0400
#define LOOKUP_RENAME_TARGET 0x0800
#define LOOKUP_PARENT      0x0010

int kern_path(const char *name, unsigned int flags, struct path *path);
struct dentry *kern_path_create(int dfd, const char *pathname,
                                struct path *path, unsigned int lookup_flags);
void done_path_create(struct path *path, struct dentry *dentry);

struct dentry *lookup_one_len(const char *name, struct dentry *base, int len);
struct dentry *lookup_one_len_unlocked(const char *name, struct dentry *base,
                                       int len);
struct dentry *lookup_one(struct mnt_idmap *idmap, const char *name,
                          struct dentry *base, int len);
struct dentry *lookup_positive_unlocked(const char *name, struct dentry *base,
                                        int len);

int vfs_path_lookup(struct dentry *dentry, struct vfsmount *mnt,
                    const char *name, unsigned int flags, struct path *path);

/* Resolve `nd_jump_link`-style symlink content. Present because ext4's fast
 * symlinks fill it in; the VFS side is what consumes it. */
struct inode *nd_get_link_inode(void);

#endif
