/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FILEATTR_H
#define LKPI_LINUX_FILEATTR_H

#include <linux/types.h>

/*
 * The per-file attribute flags behind FS_IOC_GETFLAGS / chattr: immutable,
 * append-only, no-dump, compression, no-COW.
 *
 * Both filesystems implement `fileattr_get`/`fileattr_set` and both store these
 * flags on disk, so the structure is real and the flag values are ABI —
 * `chattr +C` on a btrfs file sets FS_NOCOW_FL in the inode, and a different
 * value here would set a different attribute.
 *
 * `fsx_*` is the XFS extension. btrfs answers it; the fields have to exist.
 */

struct fileattr {
	u32 flags;
	u32 fsx_xflags;
	u32 fsx_extsize;
	u32 fsx_nextents;
	u32 fsx_projid;
	u32 fsx_cowextsize;
	bool flags_valid : 1;
	bool fsx_valid : 1;
};

void fileattr_fill_xflags(struct fileattr *fa, u32 xflags);
void fileattr_fill_flags(struct fileattr *fa, u32 flags);

static inline bool fileattr_has_fsx(const struct fileattr *fa)
{
	return fa->fsx_valid &&
	       (fa->fsx_xflags || fa->fsx_extsize != 0 || fa->fsx_projid ||
	        fa->fsx_cowextsize != 0);
}

int vfs_fileattr_get(struct dentry *dentry, struct fileattr *fa);
int vfs_fileattr_set(struct mnt_idmap *idmap, struct dentry *dentry,
                     struct fileattr *fa);

#endif
