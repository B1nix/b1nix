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

	isize (*read_cb)(struct vfs_node *node, u64 offset, char *buffer, usize size);
	usize (*list_cb)(struct vfs_node *node, const char **names, usize max_names);
	isize (*write_cb)(struct vfs_node *node, u64 offset, const char *buffer, usize size);
	int (*create_cb)(struct vfs_node *dir, const char *name, const char *full_path);
};

void vfs_init(void);
struct vfs_node *vfs_find_node(const char *path);
struct vfs_node *vfs_add_node(const char *path, enum vfs_node_type type, void *data, usize size, u32 flags);
int vfs_open(const char *path);
isize vfs_read(int handle, char *buffer, usize size);
isize vfs_write(int handle, const char *buffer, usize size);
void vfs_close(int handle);
int vfs_create(const char *path, const char *data);
int vfs_mkdir(const char *path);
usize vfs_list(const char *dir_path, const char **names, usize max_names);
struct vfs_node *vfs_find_node_by_fd(int fd);

#endif
