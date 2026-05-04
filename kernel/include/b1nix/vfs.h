#ifndef B1NIX_VFS_H
#define B1NIX_VFS_H

#include <b1nix/types.h>

#define VFS_MAX_PATH 64

enum vfs_node_type {
	VFS_FILE = 1,
	VFS_DEVICE = 2,
	VFS_DIRECTORY = 3,
};

struct vfs_node {
	char name[64];
	enum vfs_node_type type;
	u32 flags;
	usize size;
	void *data;

	struct vfs_node *parent;
	struct vfs_node *first_child;
	struct vfs_node *next_sibling;
};

void vfs_init(void);
struct vfs_node *vfs_find_node(const char *path);
int vfs_open(const char *path);
isize vfs_read(int handle, char *buffer, usize size);
isize vfs_write(int handle, const char *buffer, usize size);
void vfs_close(int handle);
int vfs_create(const char *path, const char *data);
int vfs_mkdir(const char *path);
usize vfs_list(const char *dir_path, const char **names, usize max_names);

#endif
