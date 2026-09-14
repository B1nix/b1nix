/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SECURITY_H
#define LKPI_LINUX_SECURITY_H

#include <linux/types.h>
#include <linux/errno.h>

/*
 * LSM hooks.
 *
 * b1nix has no LSM. Upstream's own !CONFIG_SECURITY stubs are what this mirrors,
 * and their return values are the contract: a hook that refuses returns an
 * error, so every stub here has to return success, and the one that produces an
 * initial security xattr has to report that there is none.
 *
 * security_inode_init_security() returns 0 when no LSM has an inode hook, which
 * is upstream's own answer for a kernel built without one (it checks
 * `blob_sizes.lbs_xattr_count` and returns success). It is NOT -EOPNOTSUPP:
 * btrfs aborts the transaction on any non-zero return, so every file creation
 * failed with -95 and left the filesystem in an error state.
 */

struct inode;
struct dentry;
struct qstr;
struct super_block;
struct fs_context;
struct security_mnt_opts;
struct xattr;

typedef int (*initxattrs)(struct inode *inode, const struct xattr *xattr_array,
                          void *fs_data);

static inline int security_inode_init_security(struct inode *inode,
                                               struct inode *dir,
                                               const struct qstr *qstr,
                                               initxattrs initxattrs_fn,
                                               void *fs_data)
{
	(void)inode; (void)dir; (void)qstr; (void)initxattrs_fn; (void)fs_data;
	return 0;
}

static inline int security_sb_set_mnt_opts(struct super_block *sb, void *mnt_opts,
                                           unsigned long kern_flags,
                                           unsigned long *set_kern_flags)
{
	(void)sb; (void)mnt_opts; (void)kern_flags; (void)set_kern_flags;
	return 0;
}

static inline int security_sb_remount(struct super_block *sb, void *mnt_opts)
{
	(void)sb; (void)mnt_opts;
	return 0;
}

static inline int security_sb_eat_lsm_opts(char *options, void **mnt_opts)
{
	(void)options; (void)mnt_opts;
	return 0;
}

static inline void security_free_mnt_opts(void **mnt_opts)
{
	(void)mnt_opts;
}

/*
 * Turning quotas on is a privileged operation an LSM may refuse. There is no
 * LSM here, so it is allowed — the same answer upstream gives with none
 * configured, and the capability check in the caller still applies.
 */
static inline int security_quota_on(struct dentry *dentry)
{ (void)dentry; return 0; }

static inline int security_quotactl(int cmds, int type, int id,
                                    struct super_block *sb)
{ (void)cmds; (void)type; (void)id; (void)sb; return 0; }

#endif
