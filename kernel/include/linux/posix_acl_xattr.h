/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_POSIX_ACL_XATTR_H
#define LKPI_LINUX_POSIX_ACL_XATTR_H

#include <linux/posix_acl.h>
#include <linux/xattr.h>

/*
 * The on-disk form of a POSIX ACL: a header plus fixed-size entries, stored as
 * an extended attribute under system.posix_acl_access / _default.
 *
 * The layout is reproduced because it is on disk — an ACL written by Linux and
 * read here has to parse — even though nothing here interprets it yet. The
 * conversion functions are declared and not defined: no object in the build
 * calls them while ACLs are off, and a stub that returned an empty ACL would be
 * a wrong answer rather than an absent one.
 */

#define POSIX_ACL_XATTR_VERSION 0x0002

struct posix_acl_xattr_entry {
	__le16 e_tag;
	__le16 e_perm;
	__le32 e_id;
};

struct posix_acl_xattr_header {
	__le32 a_version;
};

static inline size_t posix_acl_xattr_size(int count)
{
	return sizeof(struct posix_acl_xattr_header) +
	       (size_t)count * sizeof(struct posix_acl_xattr_entry);
}

static inline int posix_acl_xattr_count(size_t size)
{
	if (size < sizeof(struct posix_acl_xattr_header))
		return -1;
	size -= sizeof(struct posix_acl_xattr_header);
	if (size % sizeof(struct posix_acl_xattr_entry))
		return -1;
	return (int)(size / sizeof(struct posix_acl_xattr_entry));
}

extern const struct xattr_handler posix_acl_access_xattr_handler;
extern const struct xattr_handler posix_acl_default_xattr_handler;

#endif
