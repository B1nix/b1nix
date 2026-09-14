/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_CRC32C_H
#define LKPI_LINUX_CRC32C_H

#include <linux/types.h>

/*
 * CRC-32C (Castagnoli).
 *
 * btrfs checksums every metadata block and every data extent with it by
 * default, and ext4 checksums its superblock, group descriptors, inodes,
 * directory blocks and journal with it. A wrong implementation is not a
 * performance problem — it is a filesystem the host's own tools call corrupt.
 *
 * The seeding convention is upstream's and is not negotiable: the caller passes
 * in the running value, the function returns the new one, and both are the
 * *inverted* form. btrfs starts from ~0 and inverts the result; jbd2 and ext4
 * chain calls without inverting in between. Anything that quietly inverts here
 * would break one of the two.
 */
u32 crc32c(u32 crc, const void *address, unsigned int length);

/* Which CRC implementations have an architecture-accelerated form. These are
 * the generic table-driven ones, so none. */
#define CRC32_LE_OPTIMIZATION BIT(0)
#define CRC32_BE_OPTIMIZATION BIT(1)
#define CRC32C_OPTIMIZATION   BIT(2)
static inline u32 crc32_optimizations(void) { return 0; }

#endif
