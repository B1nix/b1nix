/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_AUDIT_H
#define LKPI_LINUX_AUDIT_H

struct filename;

/*
 * The audit subsystem's filesystem hooks.
 *
 * b1nix has its own audit log, driven from its own VFS — so an imported
 * filesystem reporting an operation here would report it a second time, after
 * the layer above has already logged it. A duplicate audit record is worse than
 * none, because a record is evidence and two of them describe two events.
 */

struct inode;
struct dentry;

#define AUDIT_TYPE_UNKNOWN      0
#define AUDIT_TYPE_NORMAL       1
#define AUDIT_TYPE_PARENT       2
#define AUDIT_TYPE_CHILD_DELETE 3
#define AUDIT_TYPE_CHILD_CREATE 4

static inline void audit_inode_child(struct inode *parent,
                                     const struct dentry *dentry,
                                     const unsigned int type)
{ (void)parent; (void)dentry; (void)type; }
static inline void audit_inode(struct filename *name,
                               const struct dentry *dentry,
                               unsigned int flags)
{ (void)name; (void)dentry; (void)flags; }

#endif
