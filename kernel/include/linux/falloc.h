/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FALLOC_H
#define LKPI_LINUX_FALLOC_H

/* The mode bits are ABI and come from the imported uapi header. */
#include <uapi/linux/falloc.h>

/* The mutually exclusive operations; the remaining bits are modifiers. */
#define FALLOC_FL_MODE_MASK (FALLOC_FL_ALLOCATE_RANGE | FALLOC_FL_PUNCH_HOLE |  \
                             FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_ZERO_RANGE |  \
                             FALLOC_FL_INSERT_RANGE | FALLOC_FL_UNSHARE_RANGE | \
                             FALLOC_FL_WRITE_ZEROES)

#endif
