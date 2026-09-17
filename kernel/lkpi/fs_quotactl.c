/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * quotactl(2) and quotactl_fd(2) on the imported filesystems.
 *
 * The command handlers are upstream's own: fs/quota/quota.c is included whole,
 * unedited, below. Its two SYSCALL_DEFINE entry points compile to functions
 * nothing calls (see <linux/syscalls.h>) because b1nix's system-call layer is
 * its own — it copies no arguments here and names the filesystem itself. What
 * this file adds is the part of those entry points that is about b1nix rather
 * than about quota: taking the superblock from the node b1nix resolved, and
 * holding s_umount across the command as quotactl_fd does.
 *
 * `addr` stays a user pointer the whole way: the handlers copy through
 * copy_to_user/copy_from_user exactly as on Linux.
 */
#include <linux/fs.h>
#include <linux/syscalls.h>

#include "quota/quota.c"

#include "fs_bridge.h"

int lkpi_bridge_quotactl(void *node, unsigned int cmd, unsigned int id,
                         void *addr, void *path_node, int path_err)
{
	struct dentry *dentry = node;
	struct super_block *sb;
	unsigned int cmds = cmd >> SUBCMDSHIFT;
	unsigned int type = cmd & SUBCMDMASK;
	struct path path;
	const struct path *pathp = ERR_PTR(-EINVAL);
	int ret;

	if (!dentry || !dentry->d_sb)
		return -ENODEV;
	if (type >= MAXQUOTAS)
		return -EINVAL;
	sb = dentry->d_sb;

	/* Q_QUOTAON names the quota file; the syscall resolved it before taking
	 * the superblock, and a failed lookup is reported only if the filesystem
	 * turns out to need the file (quota_quotaon). */
	if (cmds == Q_QUOTAON) {
		if (path_node) {
			path.mnt = NULL;
			path.dentry = path_node;
			pathp = &path;
		} else {
			pathp = ERR_PTR(path_err ? path_err : -ENOENT);
		}
	}

	if (quotactl_cmd_onoff(cmds))
		down_write(&sb->s_umount);
	else
		down_read(&sb->s_umount);
	ret = do_quotactl(sb, type, cmds, id, (void __user *)addr, pathp);
	if (quotactl_cmd_onoff(cmds))
		up_write(&sb->s_umount);
	else
		up_read(&sb->s_umount);
	return ret;
}

int lkpi_bridge_quota_enforced(void *node)
{
	struct dentry *dentry = node;
	struct super_block *sb = dentry ? dentry->d_sb : NULL;

	if (!sb)
		return 0;
	for (int type = 0; type < MAXQUOTAS; type++)
		if (sb_has_quota_limits_enabled(sb, type))
			return 1;
	return 0;
}

/* Q_SYNC with no device: every imported filesystem with quota on. */
void lkpi_bridge_quota_sync(void *root, int type)
{
	struct dentry *dentry = root;

	if (dentry && dentry->d_sb && type >= 0 && type < MAXQUOTAS)
		quota_sync_one(dentry->d_sb, &type);
}

int lkpi_bridge_quotactl_cmd_writes(unsigned int cmd)
{
	return quotactl_cmd_write((int)(cmd >> SUBCMDSHIFT));
}
