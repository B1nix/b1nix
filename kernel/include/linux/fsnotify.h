/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FSNOTIFY_H
#define LKPI_LINUX_FSNOTIFY_H

/*
 * inotify/fanotify hooks.
 *
 * b1nix has its own inotify (kernel/fs/inotify.c) driven from its own VFS, so
 * an imported filesystem calling these would report an event a second time —
 * the b1nix layer above it has already seen the operation. They are therefore
 * deliberately empty rather than forwarded: a duplicate event is worse than
 * none, because a watcher counts them.
 */

struct inode;
struct dentry;

static inline void fsnotify_create(struct inode *dir, struct dentry *dentry)
{ (void)dir; (void)dentry; }
static inline void fsnotify_link(struct inode *dir, struct inode *inode,
                                 struct dentry *new_dentry)
{ (void)dir; (void)inode; (void)new_dentry; }
static inline void fsnotify_unlink(struct inode *dir, struct dentry *dentry)
{ (void)dir; (void)dentry; }
static inline void fsnotify_mkdir(struct inode *dir, struct dentry *dentry)
{ (void)dir; (void)dentry; }
static inline void fsnotify_rmdir(struct inode *dir, struct dentry *dentry)
{ (void)dir; (void)dentry; }
static inline void fsnotify_inoderemove(struct inode *inode) { (void)inode; }
static inline void fsnotify_modify(struct file *file) { (void)file; }
static inline void fsnotify_access(struct file *file) { (void)file; }
static inline void fsnotify_change(struct dentry *dentry, unsigned int ia_valid)
{ (void)dentry; (void)ia_valid; }
static inline void fsnotify_xattr(struct dentry *dentry) { (void)dentry; }
static inline void fsnotify_move(struct inode *old_dir, struct inode *new_dir,
                                 const struct qstr *old_name,
                                 int isdir, struct inode *target,
                                 struct dentry *moved)
{ (void)old_dir; (void)new_dir; (void)old_name; (void)isdir; (void)target;
  (void)moved; }

#endif
