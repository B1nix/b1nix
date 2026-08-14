/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PSEUDO_FS_H
#define LKPI_LINUX_PSEUDO_FS_H
#include <linux/fs.h>
/* The nameless internal filesystem an anonymous inode is allocated from.
 * Wired up together with anon_inodes, when the first descriptor-returning
 * ioctl needs it. */
struct pseudo_fs_context { const struct super_operations *ops; unsigned long magic; };
struct fs_context;
static inline struct pseudo_fs_context *init_pseudo(struct fs_context *fc,
                                                    unsigned long magic)
{ (void)fc; (void)magic; return 0; }
#endif
