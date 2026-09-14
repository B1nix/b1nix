/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FIEMAP_H
#define LKPI_LINUX_FIEMAP_H

#include <linux/types.h>
#include <linux/errno.h>

/*
 * FIEMAP: "where does this file physically live?"
 *
 * The kernel side of the ioctl. Userspace passes an array of extent slots and
 * the filesystem fills them in; `fi_extents_mapped` is how many it has written
 * and `fi_extents_max` how many there is room for — a filesystem that ignores
 * the second overruns a userspace buffer.
 *
 * The FIEMAP_EXTENT_* flags are ABI: `filefrag -v` prints them.
 */

struct fiemap_extent {
	__u64 fe_logical;
	__u64 fe_physical;
	__u64 fe_length;
	__u64 fe_reserved64[2];
	__u32 fe_flags;
	__u32 fe_reserved[3];
};

struct fiemap_extent_info {
	unsigned int fi_flags;
	unsigned int fi_extents_mapped;
	unsigned int fi_extents_max;
	struct fiemap_extent __user *fi_extents_start;
};

#define FIEMAP_MAX_OFFSET (~0ULL)
#define FIEMAP_FLAG_SYNC  0x0001
#define FIEMAP_FLAG_XATTR 0x0002
#define FIEMAP_FLAG_CACHE 0x0004
#define FIEMAP_FLAGS_COMPAT (FIEMAP_FLAG_SYNC | FIEMAP_FLAG_XATTR)

#define FIEMAP_EXTENT_LAST           0x0001
#define FIEMAP_EXTENT_UNKNOWN        0x0002
#define FIEMAP_EXTENT_DELALLOC       0x0004
#define FIEMAP_EXTENT_ENCODED        0x0008
#define FIEMAP_EXTENT_DATA_ENCRYPTED 0x0080
#define FIEMAP_EXTENT_NOT_ALIGNED    0x0100
#define FIEMAP_EXTENT_DATA_INLINE    0x0200
#define FIEMAP_EXTENT_DATA_TAIL      0x0400
#define FIEMAP_EXTENT_UNWRITTEN      0x0800
#define FIEMAP_EXTENT_MERGED         0x1000
#define FIEMAP_EXTENT_SHARED         0x2000

int fiemap_prep(struct inode *inode, struct fiemap_extent_info *fieinfo,
                u64 start, u64 *len, u32 supported_flags);
int fiemap_fill_next_extent(struct fiemap_extent_info *info, u64 logical,
                            u64 phys, u64 len, u32 flags);

/*
 * The ioctl argument itself, as userspace passes it.
 *
 * `fm_extent_count` is what userspace has room for and `fm_mapped_extents` is
 * what the kernel filled in — a filesystem that writes more than the first
 * overruns a userspace buffer, which is why both are here rather than one
 * length.
 */
struct fiemap {
	__u64 fm_start;
	__u64 fm_length;
	__u32 fm_flags;
	__u32 fm_mapped_extents;
	__u32 fm_extent_count;
	__u32 fm_reserved;
	struct fiemap_extent fm_extents[];
};

#endif
