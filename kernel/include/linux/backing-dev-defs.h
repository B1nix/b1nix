/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BACKING_DEV_DEFS_H
#define LKPI_LINUX_BACKING_DEV_DEFS_H

/*
 * Upstream splits the bdi's type definitions from its functions. The shim's
 * <linux/backing-dev.h> is small enough to be both; FAT includes this half to
 * read the read-ahead window off sb->s_bdi.
 */
#include <linux/backing-dev.h>

#endif
