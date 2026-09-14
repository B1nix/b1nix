/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PATH_H
#define LKPI_LINUX_PATH_H

struct vfsmount;
struct dentry;

/*
 * A resolved path is a (mount, dentry) pair, never a dentry alone: the same
 * directory reached through two mounts is two different paths, and the mount is
 * what says which one. Both members are counted references, which is what
 * path_get and path_put maintain.
 */
struct path {
	struct vfsmount *mnt;
	struct dentry *dentry;
};

void path_get(const struct path *path);
void path_put(const struct path *path);

static inline int path_equal(const struct path *path1, const struct path *path2)
{
	return path1->mnt == path2->mnt && path1->dentry == path2->dentry;
}

#endif
