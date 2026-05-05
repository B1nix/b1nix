#ifndef B1NIX_VFS_H
#define B1NIX_VFS_H

#include <b1nix/types.h>
#include <b1nix/uidgid.h>

#define VFS_MAX_PATH 64

/* Standard permission bits */
#define VFS_IRUSR  0400    /* Owner read */
#define VFS_IWUSR  0200    /* Owner write */
#define VFS_IXUSR  0100    /* Owner execute */
#define VFS_IRGRP  0040    /* Group read */
#define VFS_IWGRP  0020    /* Group write */
#define VFS_IXGRP  0010    /* Group execute */
#define VFS_IROTH  0004    /* Others read */
#define VFS_IWOTH  0002    /* Others write */
#define VFS_IXOTH  0001    /* Others execute */

#define VFS_DEFAULT_PERMS (VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IROTH)

enum vfs_node_type {
	VFS_FILE = 1,
	VFS_DEVICE = 2,
	VFS_DIRECTORY = 3,
};

struct acl_entry {
	u16 tag;      /* ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_GROUP, ACL_MASK, ACL_OTHER */
	u16 qualifier; /* UID or GID (for ACL_USER, ACL_GROUP) */
	u16 perms;    /* Permission bitmask */
};

#define ACL_MAX_ENTRIES 8

/* ACL tag types */
#define ACL_USER_OBJ    0x01
#define ACL_USER        0x02
#define ACL_GROUP_OBJ   0x04
#define ACL_GROUP       0x08
#define ACL_MASK        0x10
#define ACL_OTHER       0x20

struct vfs_node {
	char name[64];
	enum vfs_node_type type;
	u32 flags;
	usize size;
	void *data;

	/* Ownership and permissions */
	u16 uid;
	u16 gid;
	u16 mode;        /* Permission bits: rwxrwxrwx + type bits */

	/* ACL support */
	struct acl_entry acls[ACL_MAX_ENTRIES];
	int acl_count;

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

/* Permission-aware operations */
int vfs_open(const char *path);
isize vfs_read(int handle, char *buffer, usize size);
isize vfs_write(int handle, const char *buffer, usize size);
void vfs_close(int handle);
int vfs_create(const char *path, const char *data);
int vfs_mkdir(const char *path);
usize vfs_list(const char *dir_path, const char **names, usize max_names);
struct vfs_node *vfs_find_node_by_fd(int fd);

/* Permission management */
int vfs_chmod(const char *path, u16 mode);
int vfs_chown(const char *path, u16 uid, u16 gid);
int vfs_get_node_perm(const struct vfs_node *node, const struct cred *cred, int write_access);
int vfs_set_acl(struct vfs_node *node, const struct acl_entry *acl);
int vfs_get_acl(struct vfs_node *node, struct acl_entry *out_acl, int max_entries);

#endif

