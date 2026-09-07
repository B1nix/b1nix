/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef LKPI_UAPI_LINUX_XATTR_H
#define LKPI_UAPI_LINUX_XATTR_H

/*
 * The extended-attribute namespace, which is ABI: these prefixes appear on
 * disk in every ext4 and btrfs filesystem, and userspace passes these names to
 * getxattr/setxattr. Reproduced from upstream rather than invented.
 */

#define XATTR_CREATE  0x1  /* fail if the attribute already exists */
#define XATTR_REPLACE 0x2  /* fail if it does not */

#define XATTR_OS2_PREFIX "os2."
#define XATTR_OS2_PREFIX_LEN (sizeof(XATTR_OS2_PREFIX) - 1)

#define XATTR_MAC_OSX_PREFIX "osx."
#define XATTR_MAC_OSX_PREFIX_LEN (sizeof(XATTR_MAC_OSX_PREFIX) - 1)

#define XATTR_BTRFS_PREFIX "btrfs."
#define XATTR_BTRFS_PREFIX_LEN (sizeof(XATTR_BTRFS_PREFIX) - 1)

#define XATTR_HURD_PREFIX "gnu."
#define XATTR_HURD_PREFIX_LEN (sizeof(XATTR_HURD_PREFIX) - 1)

#define XATTR_SECURITY_PREFIX "security."
#define XATTR_SECURITY_PREFIX_LEN (sizeof(XATTR_SECURITY_PREFIX) - 1)

#define XATTR_SYSTEM_PREFIX "system."
#define XATTR_SYSTEM_PREFIX_LEN (sizeof(XATTR_SYSTEM_PREFIX) - 1)

#define XATTR_TRUSTED_PREFIX "trusted."
#define XATTR_TRUSTED_PREFIX_LEN (sizeof(XATTR_TRUSTED_PREFIX) - 1)

#define XATTR_USER_PREFIX "user."
#define XATTR_USER_PREFIX_LEN (sizeof(XATTR_USER_PREFIX) - 1)

#define XATTR_NAME_POSIX_ACL_ACCESS  XATTR_SYSTEM_PREFIX "posix_acl_access"
#define XATTR_NAME_POSIX_ACL_DEFAULT XATTR_SYSTEM_PREFIX "posix_acl_default"
#define XATTR_NAME_CAPS              XATTR_SECURITY_PREFIX "capability"
#define XATTR_NAME_SELINUX           XATTR_SECURITY_PREFIX "selinux"
#define XATTR_NAME_SMACK             XATTR_SECURITY_PREFIX "SMACK64"
#define XATTR_NAME_SMACKTRANSMUTE    XATTR_SECURITY_PREFIX "SMACK64TRANSMUTE"

#define XATTR_NAME_MAX 255      /* the attribute name, without the trailing NUL */
#define XATTR_SIZE_MAX 65536    /* the value */
#define XATTR_LIST_MAX 65536    /* a listxattr result */

#endif
