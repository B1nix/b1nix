/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_HIGHUID_H
#define LKPI_LINUX_HIGHUID_H

#include <linux/types.h>

/*
 * 16-bit uid/gid conversion.
 *
 * ext2 and ext4 store a 16-bit uid in the inode plus a high half in a separate
 * field, so writing an inode means splitting a 32-bit id and reading one means
 * reassembling it. The overflow ids are what a 32-bit id becomes when it will
 * not fit — upstream's defaults, and they are what an old e2fsck expects to see.
 */

#define DEFAULT_OVERFLOWUID 65534
#define DEFAULT_OVERFLOWGID 65534

extern int overflowuid;
extern int overflowgid;
extern int fs_overflowuid;
extern int fs_overflowgid;

#define high2lowuid(uid) ((uid) > 65535 ? (uid_t)overflowuid : (uid_t)(uid))
#define high2lowgid(gid) ((gid) > 65535 ? (gid_t)overflowgid : (gid_t)(gid))
#define low2highuid(uid) ((uid) == (uid_t)-1 ? (uid_t)-1 : (uid_t)(uid))
#define low2highgid(gid) ((gid) == (gid_t)-1 ? (gid_t)-1 : (gid_t)(gid))

#define fs_high2lowuid(uid) ((uid) > 65535 ? (uid_t)fs_overflowuid : (uid_t)(uid))
#define fs_high2lowgid(gid) ((gid) > 65535 ? (gid_t)fs_overflowgid : (gid_t)(gid))

#define SET_UID16(var, uid) do { (var) = fs_high2lowuid(uid); } while (0)
#define SET_GID16(var, gid) do { (var) = fs_high2lowgid(gid); } while (0)

#endif
