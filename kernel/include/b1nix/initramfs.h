#ifndef B1NIX_INITRAMFS_H
#define B1NIX_INITRAMFS_H

#include <b1nix/types.h>

#define INITRAMFS_EXECUTABLE 0x1

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
