/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_FS_BRIDGE_H
#define LKPI_FS_BRIDGE_H

/*
 * The bridge's interface, and the only thing both sides of it include.
 *
 * Every type here is a plain C type on purpose: kernel/lkpi/fs_bridge.c is
 * compiled against Linux's headers and kernel/fs/lkpifs.c against b1nix's, and
 * the two disagree about `struct inode`, `spinlock_t` and `current`. A handle
 * is an opaque pointer — a `struct dentry *` on the Linux side, and nothing the
 * b1nix side may look inside.
 */

struct lkpi_bridge_attr {
	unsigned long long ino;
	unsigned long long size;
	unsigned long long blocks;
	unsigned int mode;
	unsigned int nlink;
	unsigned int uid;
	unsigned int gid;
	unsigned long long atime;
	unsigned long long mtime;
	unsigned long long ctime;
};

/* Called once per directory entry. Return 0 to stop the walk. */
typedef int (*lkpi_bridge_emit_fn)(void *arg, const char *name, int len,
                                   unsigned long long ino, unsigned int type);

/* Mount `source` with the imported filesystem `fstype`; NULL on failure. The
 * handle returned is the root. */
void *lkpi_bridge_mount(const char *fstype, const char *source,
                        unsigned long flags);
void lkpi_bridge_unmount(void *root);

void *lkpi_bridge_lookup(void *dir, const char *name);
void lkpi_bridge_put(void *node);
int lkpi_bridge_attr(void *node, struct lkpi_bridge_attr *out);

long lkpi_bridge_read(void *node, unsigned long long off, char *buf,
                      unsigned long len);
long lkpi_bridge_write(void *node, unsigned long long off, const char *buf,
                       unsigned long len);
int lkpi_bridge_sync(void *node);
int lkpi_bridge_sync_fs(void *root);

int lkpi_bridge_iterate(void *dir, unsigned long long cookie,
                        unsigned long long *next, lkpi_bridge_emit_fn emit,
                        void *arg);

int lkpi_bridge_create(void *dir, const char *name, unsigned int mode);
int lkpi_bridge_mkdir(void *dir, const char *name, unsigned int mode);
int lkpi_bridge_symlink(void *dir, const char *name, const char *target);
int lkpi_bridge_link(void *dir, const char *name, void *target);
int lkpi_bridge_unlink(void *dir, const char *name);
int lkpi_bridge_rmdir(void *dir, const char *name);
int lkpi_bridge_rename(void *olddir, const char *oldname, void *newdir,
                       const char *newname);
int lkpi_bridge_readlink(void *node, char *buf, unsigned long len);
int lkpi_bridge_truncate(void *node, unsigned long long size);
int lkpi_bridge_chmod(void *node, unsigned int mode);

/* Extended attributes. A negative return is -errno; getxattr and listxattr
 * return the size, and accept a NULL buffer to ask for it. */
long lkpi_bridge_getxattr(void *node, const char *name, void *value,
                          unsigned long size);
int lkpi_bridge_setxattr(void *node, const char *name, const void *value,
                         unsigned long size, int flags);
int lkpi_bridge_removexattr(void *node, const char *name);
long lkpi_bridge_listxattr(void *node, char *list, unsigned long size);

#endif /* LKPI_FS_BRIDGE_H */
