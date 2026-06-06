#ifndef B1NIX_INITRAMFS_H
#define B1NIX_INITRAMFS_H

#include <b1nix/types.h>

#define INITRAMFS_EXECUTABLE 0x1
/* M31: set the S_ISUID bit on the initramfs inode so an execve of the file
 * by an unprivileged user elevates the new task's euid to the file's
 * owner (uid 0 — initramfs files are root-owned). Used for /bin/su. */
#define INITRAMFS_SETUID     0x2
#define INITRAMFS_SYMLINK    0x4

struct initramfs_file {
	const char *path;
	const char *data;
	usize size;
	u32 flags;
};

void initramfs_init(void);
const struct initramfs_file *initramfs_find(const char *path);
const struct initramfs_file *initramfs_get(usize index);
usize initramfs_count(void);

#endif
