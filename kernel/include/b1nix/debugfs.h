/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_DEBUGFS_H
#define B1NIX_DEBUGFS_H

#include <b1nix/types.h>
#include <b1nix/vfs.h>

void b1nix_debugfs_init(void);

struct vfs_node *b1nix_debugfs_create_dir(const char *name, struct vfs_node *parent);
struct vfs_node *b1nix_debugfs_create_file(const char *name, u16 mode,
                                           struct vfs_node *parent, void *data,
                                           isize (*read_cb)(struct vfs_node *node, u64 offset,
                                                            char *buf, usize size, int flags));
struct vfs_node *b1nix_debugfs_create_u32(const char *name, u16 mode,
                                         struct vfs_node *parent, u32 *value);
struct vfs_node *b1nix_debugfs_create_bool(const char *name, u16 mode,
                                          struct vfs_node *parent, int *value);

#endif
