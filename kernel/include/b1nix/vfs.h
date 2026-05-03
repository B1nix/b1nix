#ifndef B1NIX_VFS_H
#define B1NIX_VFS_H

#include <b1nix/types.h>

#define VFS_MAX_PATH 64

enum vfs_node_type {
	VFS_FILE = 1,
	VFS_DEVICE = 2,
};

struct vfs_node {
	const char *path;
	enum vfs_node_type type;
	u32 flags;
	usize size;
	void *data;
};

void vfs_init(void);
int vfs_open(const char *path);
isize vfs_read(int handle, char *buffer, usize size);
isize vfs_write(int handle, const char *buffer, usize size);
void vfs_close(int handle);
int vfs_create(const char *path, const char *data);
usize vfs_list(const char **paths, usize max_paths);

#endif
