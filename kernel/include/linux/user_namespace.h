/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_USER_NAMESPACE_H
#define LKPI_LINUX_USER_NAMESPACE_H

/*
 * User namespaces, as far as an imported filesystem sees them: the initial
 * namespace and the id mapping functions, which <linux/uidgid.h> carries.
 * isofs includes this header for init_user_ns, against which its uid= and
 * gid= mount options are printed. Every superblock here belongs to the
 * initial namespace (b1nix's own user namespaces live on the b1nix side of
 * the bridge), so nothing more is declared.
 */
#include <linux/uidgid.h>

#endif
