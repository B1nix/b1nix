/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_ANON_INODES_H
#define LKPI_LINUX_ANON_INODES_H
#include <linux/types.h>
/* An anonymous file: a descriptor with no name in any directory, which is how
 * a fence or a dma-buf is handed to userspace. b1nix's VFS can make one, but
 * which of its paths the DRM core should use is decided when the first ioctl
 * that returns a descriptor is wired up. */
struct file;
struct file_operations;
int anon_inode_getfd(const char *name, const struct file_operations *fops,
                     void *priv, int flags);
struct file *anon_inode_getfile(const char *name,
                                const struct file_operations *fops, void *priv,
                                int flags);
#endif
