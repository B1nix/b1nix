/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_STATFS_H
#define LKPI_LINUX_STATFS_H

#include <linux/types.h>

/*
 * What `statfs` reports. The kernel-internal form: the syscall layer converts
 * it to whichever userspace structure was asked for.
 *
 * `f_fsid` is two 32-bit words rather than a 64-bit one because that is what
 * the ABI has always been, and both filesystems fill it from a hash of their
 * UUID.
 */

typedef struct {
	int val[2];
} __kernel_fsid_t;

/* The 64-bit id a filesystem without a UUID (FAT: its volume serial) folds
 * into the two words. */
static inline __kernel_fsid_t u64_to_fsid(u64 v)
{
	__kernel_fsid_t f = { .val = { (int)(u32)v, (int)(u32)(v >> 32) } };

	return f;
}

struct kstatfs {
	long f_type;
	long f_bsize;
	u64 f_blocks;
	u64 f_bfree;
	u64 f_bavail;
	u64 f_files;
	u64 f_ffree;
	__kernel_fsid_t f_fsid;
	long f_namelen;
	long f_frsize;
	long f_flags;
	long f_spare[4];
};

#endif
