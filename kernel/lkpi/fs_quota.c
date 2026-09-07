/* SPDX-License-Identifier: MIT */
/*
 * The two symbols the imported quota core needs from outside itself.
 *
 * fs/quota/{dquot,quota_tree,quota_v2,kqid}.c are imported; fs/quota/quota.c is
 * not, because it is the quotactl(2) system call and b1nix has its own syscall
 * layer. dquot.c calls one helper that happens to live in that file, and the
 * quota core registers a sysctl table whose handler b1nix has no equivalent of.
 */

#include <linux/fs.h>
#include <linux/quota.h>
#include <linux/sysctl.h>
#include <linux/errno.h>

/*
 * Which enforcement flag belongs to a quota type. Upstream keeps this in
 * quota.c next to the system call; it is a mapping between two of that
 * subsystem's own constants, and the alternative to stating it here is
 * importing the whole system call for one switch.
 */
unsigned int qtype_enforce_flag(int type)
{
	switch (type) {
	case USRQUOTA:
		return FS_QUOTA_UDQ_ENFD;
	case GRPQUOTA:
		return FS_QUOTA_GDQ_ENFD;
	case PRJQUOTA:
		return FS_QUOTA_PDQ_ENFD;
	}
	return 0;
}

/*
 * The handler for the quota core's own /proc/sys entries. Nothing registers
 * the table (see <linux/sysctl.h>), so this is never reached; it exists
 * because a table names its handler, and a table with a NULL one would be a
 * fault waiting for the day the tree appears.
 */
int proc_doulongvec_minmax(struct ctl_table *table, int write, void *buffer,
                           size_t *lenp, loff_t *ppos)
{
	(void)table; (void)write; (void)buffer; (void)lenp; (void)ppos;
	return -ENOSYS;
}
