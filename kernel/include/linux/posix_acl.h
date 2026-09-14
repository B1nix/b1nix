/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_POSIX_ACL_H
#define LKPI_LINUX_POSIX_ACL_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/atomic.h>

/*
 * POSIX access control lists.
 *
 * b1nix carries permission bits and no ACLs, and both filesystems are built
 * without `*_FS_POSIX_ACL`, so acl.c is not in either object list. What is left
 * is the handful of declarations the rest of the filesystem references
 * unconditionally — the type, the cache slot in the inode, and the "no ACL"
 * sentinel.
 *
 * The sentinel is the part that must not be simplified: `ACL_NOT_CACHED` and a
 * NULL pointer mean different things. NULL means "this inode has no ACL",
 * which is an answer; ACL_NOT_CACHED means "nobody has looked yet", which is
 * not. Collapsing them makes every inode look like it has been checked.
 */

struct posix_acl_entry {
	short e_tag;
	unsigned short e_perm;
	union {
		kuid_t e_uid;
		kgid_t e_gid;
	};
};

struct posix_acl {
	refcount_t a_refcount;
	struct rcu_head a_rcu;
	unsigned int a_count;
	struct posix_acl_entry a_entries[];
};

#define ACL_UNDEFINED_ID (-1)

/* e_tag values, which are on disk. */
#define ACL_USER_OBJ  (0x01)
#define ACL_USER      (0x02)
#define ACL_GROUP_OBJ (0x04)
#define ACL_GROUP     (0x08)
#define ACL_MASK      (0x10)
#define ACL_OTHER     (0x20)

#define ACL_TYPE_ACCESS  (0x8000)
#define ACL_TYPE_DEFAULT (0x4000)

#define ACL_NOT_CACHED ((void *)(-1))
#define ACL_DONT_CACHE ((void *)(-3))

struct inode;
struct dentry;
struct mnt_idmap;

static inline struct posix_acl *posix_acl_dup(struct posix_acl *acl)
{ return acl; }
static inline void posix_acl_release(struct posix_acl *acl) { (void)acl; }
static inline void forget_cached_acl(struct inode *inode, int type)
{ (void)inode; (void)type; }
static inline void forget_all_cached_acls(struct inode *inode) { (void)inode; }
static inline int posix_acl_chmod(struct mnt_idmap *idmap, struct dentry *dentry,
                                  umode_t mode)
{ (void)idmap; (void)dentry; (void)mode; return 0; }
static inline int posix_acl_create(struct inode *inode, umode_t *mode,
                                   struct posix_acl **default_acl,
                                   struct posix_acl **acl)
{
	(void)inode; (void)mode;
	*default_acl = NULL;
	*acl = NULL;
	return 0;
}
static inline struct posix_acl *get_inode_acl(struct inode *inode, int type)
{ (void)inode; (void)type; return NULL; }
static inline void set_cached_acl(struct inode *inode, int type,
                                  struct posix_acl *acl)
{ (void)inode; (void)type; (void)acl; }

#endif
