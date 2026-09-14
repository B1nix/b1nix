/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_UTSNAME_H
#define LKPI_LINUX_UTSNAME_H

#include <linux/types.h>

/*
 * The system's names. ext4 records the nodename in its superblock's
 * "last mounted by" fields, which is why a filesystem needs this at all.
 *
 * b1nix has no UTS namespaces, so there is one instance and `init_utsname`
 * returns it. The field sizes are ABI: 65 bytes each, and a longer name is
 * truncated rather than overflowing.
 */

#define __NEW_UTS_LEN 64

struct new_utsname {
	char sysname[__NEW_UTS_LEN + 1];
	char nodename[__NEW_UTS_LEN + 1];
	char release[__NEW_UTS_LEN + 1];
	char version[__NEW_UTS_LEN + 1];
	char machine[__NEW_UTS_LEN + 1];
	char domainname[__NEW_UTS_LEN + 1];
};

struct uts_namespace {
	struct new_utsname name;
};

struct new_utsname *init_utsname(void);
struct new_utsname *utsname(void);

#endif
