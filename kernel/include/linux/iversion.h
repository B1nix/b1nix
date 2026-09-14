/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_IVERSION_H
#define LKPI_LINUX_IVERSION_H

#include <linux/fs.h>
#include <linux/atomic.h>

/*
 * The inode change counter, i_version.
 *
 * NFS and IMA use it to detect that a file changed without comparing its
 * contents; ext4 and btrfs maintain it because it is stored on disk and an
 * fsck compares it. Upstream keeps a "queried" flag in the low bit so an inode
 * nobody has asked about can skip the increment; that optimisation is kept,
 * because the on-disk value differs depending on it and a version that
 * increments on every write where Linux's would not is a diff every check
 * reports.
 */

#define I_VERSION_QUERIED_SHIFT 1
#define I_VERSION_QUERIED       1ULL
#define I_VERSION_INCREMENT     (1ULL << I_VERSION_QUERIED_SHIFT)

static inline void inode_set_iversion_raw(struct inode *inode, u64 val)
{
	atomic64_set(&inode->i_version, (long long)val);
}

static inline u64 inode_peek_iversion_raw(const struct inode *inode)
{
	return (u64)atomic64_read(&((struct inode *)inode)->i_version);
}

static inline void inode_set_iversion(struct inode *inode, u64 val)
{
	inode_set_iversion_raw(inode, val << I_VERSION_QUERIED_SHIFT);
}

static inline void inode_set_iversion_queried(struct inode *inode, u64 val)
{
	inode_set_iversion_raw(inode,
	                       (val << I_VERSION_QUERIED_SHIFT) | I_VERSION_QUERIED);
}

static inline u64 inode_peek_iversion(const struct inode *inode)
{
	return inode_peek_iversion_raw(inode) >> I_VERSION_QUERIED_SHIFT;
}

/* Bump it, but only if somebody has looked since the last bump. Returns whether
 * the counter actually moved. */
static inline bool inode_maybe_inc_iversion(struct inode *inode, bool force)
{
	u64 cur = inode_peek_iversion_raw(inode);

	if (!force && !(cur & I_VERSION_QUERIED))
		return false;
	inode_set_iversion_raw(inode, (cur & ~I_VERSION_QUERIED) + I_VERSION_INCREMENT);
	return true;
}

static inline void inode_inc_iversion(struct inode *inode)
{
	inode_maybe_inc_iversion(inode, true);
}

/* Reading it is what sets the queried flag; that is the whole mechanism. */
static inline u64 inode_query_iversion(struct inode *inode)
{
	u64 cur = inode_peek_iversion_raw(inode);

	inode_set_iversion_raw(inode, cur | I_VERSION_QUERIED);
	return cur >> I_VERSION_QUERIED_SHIFT;
}

static inline bool inode_eq_iversion_raw(const struct inode *inode, u64 old)
{
	return inode_peek_iversion_raw(inode) == old;
}

static inline bool inode_eq_iversion(const struct inode *inode, u64 old)
{
	return inode_peek_iversion(inode) == old;
}

#endif
