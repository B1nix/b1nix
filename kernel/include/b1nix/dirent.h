#ifndef B1NIX_DIRENT_H
#define B1NIX_DIRENT_H

#include <b1nix/types.h>

/* Directory entry returned by SYS_READDIR */
struct dirent {
	char  name[64];   /* File/directory name */
	u32   type;        /* 1 = file, 2 = device, 3 = directory */
	u32   is_dir;      /* 1 if directory */
	u32   is_exec;     /* 1 if executable */
	u64   size;        /* File size */
	/* Inode number of the entry, as the filesystem knows it — this is what
	 * getdents(2)/getdents64(2) report as d_ino. Filesystems without inode
	 * numbers (FAT, exFAT, ISO 9660) put a stable per-file identity here
	 * instead (its first cluster / extent LBA), which is what those entries
	 * have in place of an inode. 0 means "unknown"; the syscall layer then
	 * falls back to the entry's directory index so d_ino is never 0 (some
	 * tools read 0 as "deleted"). */
	u64   ino;
};

#endif
