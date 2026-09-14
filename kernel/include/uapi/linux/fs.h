/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef LKPI_UAPI_LINUX_FS_H
#define LKPI_UAPI_LINUX_FS_H

#include <linux/types.h>

/*
 * The filesystem ABI userspace speaks: ioctl argument structures and the
 * per-file attribute flags.
 *
 * Every value here is fixed by what userspace already sends. `fstrim_range` is
 * what `fstrim(8)` passes to FITRIM; the FS_*_FL flags are what `chattr` sets
 * and what both filesystems store on disk. Reproduced from upstream rather than
 * invented — a different bit for FS_NOCOW_FL would silently set a different
 * attribute on every file.
 */

struct fstrim_range {
	__u64 start;
	__u64 len;
	__u64 minlen;   /* do not bother trimming a free run shorter than this */
};

struct file_clone_range {
	__s64 src_fd;
	__u64 src_offset;
	__u64 src_length;
	__u64 dest_offset;
};

struct file_dedupe_range_info {
	__s64 dest_fd;
	__u64 dest_offset;
	__u64 bytes_deduped;
	__s32 status;
	__u32 reserved;
};

struct file_dedupe_range {
	__u64 src_offset;
	__u64 src_length;
	__u16 dest_count;
	__u16 reserved1;
	__u32 reserved2;
	struct file_dedupe_range_info info[];
};

struct files_stat_struct {
	unsigned long nr_files;
	unsigned long nr_free_files;
	unsigned long max_files;
};

struct inodes_stat_t {
	long nr_inodes;
	long nr_unused;
	long dummy[5];
};

/* Per-file attribute flags: FS_IOC_GETFLAGS / SETFLAGS. */
#define FS_SECRM_FL        0x00000001
#define FS_UNRM_FL         0x00000002
#define FS_COMPR_FL        0x00000004
#define FS_SYNC_FL         0x00000008
#define FS_IMMUTABLE_FL    0x00000010
#define FS_APPEND_FL       0x00000020
#define FS_NODUMP_FL       0x00000040
#define FS_NOATIME_FL      0x00000080
#define FS_DIRTY_FL        0x00000100
#define FS_COMPRBLK_FL     0x00000200
#define FS_NOCOMP_FL       0x00000400
#define FS_ENCRYPT_FL      0x00000800
#define FS_BTREE_FL        0x00001000
#define FS_INDEX_FL        0x00001000
#define FS_IMAGIC_FL       0x00002000
#define FS_JOURNAL_DATA_FL 0x00004000
#define FS_NOTAIL_FL       0x00008000
#define FS_DIRSYNC_FL      0x00010000
#define FS_TOPDIR_FL       0x00020000
#define FS_HUGE_FILE_FL    0x00040000
#define FS_EXTENT_FL       0x00080000
#define FS_VERITY_FL       0x00100000
#define FS_EA_INODE_FL     0x00200000
#define FS_EOFBLOCKS_FL    0x00400000
#define FS_NOCOW_FL        0x00800000
#define FS_DAX_FL          0x02000000
#define FS_INLINE_DATA_FL  0x10000000
#define FS_PROJINHERIT_FL  0x20000000
#define FS_CASEFOLD_FL     0x40000000
#define FS_RESERVED_FL     0x80000000

#define FS_FL_USER_VISIBLE    0x0003DFFF
#define FS_FL_USER_MODIFIABLE 0x000380FF

/* The xflags form, from XFS, which both filesystems also answer. */
#define FS_XFLAG_REALTIME     0x00000001
#define FS_XFLAG_PREALLOC     0x00000002
#define FS_XFLAG_IMMUTABLE    0x00000008
#define FS_XFLAG_APPEND       0x00000010
#define FS_XFLAG_SYNC         0x00000020
#define FS_XFLAG_NOATIME      0x00000040
#define FS_XFLAG_NODUMP       0x00000080
#define FS_XFLAG_RTINHERIT    0x00000100
#define FS_XFLAG_PROJINHERIT  0x00000200
#define FS_XFLAG_NOSYMLINKS   0x00000400
#define FS_XFLAG_EXTSIZE      0x00000800
#define FS_XFLAG_EXTSZINHERIT 0x00001000
#define FS_XFLAG_NODEFRAG     0x00002000
#define FS_XFLAG_FILESTREAM   0x00004000
#define FS_XFLAG_DAX          0x00008000
#define FS_XFLAG_COWEXTSIZE   0x00010000
#define FS_XFLAG_HASATTR      0x80000000

/* Mount flags as userspace passes them, and the SEEK_* / RWF_* families. */
#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT     32
#define MS_MANDLOCK    64
#define MS_DIRSYNC     128
#define MS_NOATIME     1024
#define MS_NODIRATIME  2048
#define MS_BIND        4096
#define MS_MOVE        8192
#define MS_REC         16384
#define MS_SILENT      32768
#define MS_POSIXACL    (1 << 16)
#define MS_LAZYTIME    (1 << 25)
#define MS_ACTIVE      (1 << 30)

#define RWF_HIPRI  0x00000001
#define RWF_DSYNC  0x00000002
#define RWF_SYNC   0x00000004
#define RWF_NOWAIT 0x00000008
#define RWF_APPEND 0x00000010

#endif
