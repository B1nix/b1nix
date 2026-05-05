#include <string.h>
#include <b1nix/console.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/blk.h>
#include <b1nix/fat32.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>

#define MAX_VFS_NODES 256
#define MAX_VFS_HANDLES 64

struct vfs_handle {
	int used;
	struct vfs_node *node;
	usize offset;
};

static struct vfs_node nodes[MAX_VFS_NODES];
static struct vfs_handle handles[MAX_VFS_HANDLES];
static usize node_count = 0;
static struct vfs_node *root_node = 0;

void virtio_blk_init(void);

/* ── Permission Helpers ── */

extern struct cred *scheduler_get_current_cred(void);

int vfs_get_node_perm(const struct vfs_node *node, const struct cred *cred, int write_access)
{
	if (!node || !cred) return 0;

	/* Root can do anything */
	if (cred->euid == ROOT_UID) return 1;

	/* Check DAC_OVERRIDE capability */
	if (cred_has_cap(cred, CAP_DAC_OVERRIDE)) return 1;
	if (!write_access && cred_has_cap(cred, CAP_DAC_READ_SEARCH)) return 1;

	/* Check ACLs first if present */
	if (node->acl_count > 0) {
		u16 matched_perms = 0;
		int mask_found = 0;
		u16 mask_perms = 0;
		
		for (int i = 0; i < node->acl_count; i++) {
			if (node->acls[i].tag == ACL_MASK) {
				mask_found = 1;
				mask_perms = node->acls[i].perms;
			}
		}

		for (int i = 0; i < node->acl_count; i++) {
			switch (node->acls[i].tag) {
			case ACL_USER_OBJ:
				if (cred->euid == node->uid) {
					matched_perms = node->acls[i].perms;
					goto acl_check;
				}
				break;
			case ACL_USER:
				if (cred->euid == node->acls[i].qualifier) {
					matched_perms = node->acls[i].perms;
					goto acl_check;
				}
				break;
			case ACL_GROUP_OBJ:
				if (cred->egid == node->gid) {
					matched_perms = node->acls[i].perms;
					goto acl_check;
				}
				break;
			case ACL_GROUP:
				if (cred->egid == node->acls[i].qualifier) {
					matched_perms = node->acls[i].perms;
					goto acl_check;
				}
				for (int g = 0; g < cred->ngroups; g++) {
					if (cred->groups[g] == node->acls[i].qualifier) {
						matched_perms = node->acls[i].perms;
						goto acl_check;
					}
				}
				break;
			}
		}

	acl_check:
		if (mask_found) matched_perms &= mask_perms;
		if (write_access) return (matched_perms & 0200) != 0;
		return (matched_perms & 0400) != 0;
	}

	/* Standard Unix permissions */
	return cred_can_access(cred, node->uid, node->gid, node->mode, write_access);
}

static const struct cred *get_current_cred(void)
{
	return scheduler_get_current_cred();
}

/* ── Node allocation with default permissions ── */

static struct vfs_node *alloc_node(void)
{
	if (node_count >= MAX_VFS_NODES) {
		return 0;
	}
	struct vfs_node *node = &nodes[node_count++];
	memset(node, 0, sizeof(*node));
	const struct cred *cred = get_current_cred();
	if (cred) {
		node->uid = cred->euid;
		node->gid = cred->egid;
	} else {
		node->uid = ROOT_UID;
		node->gid = ROOT_GID;
	}
	node->mode = VFS_DEFAULT_PERMS;
	return node;
}

static void split_path(const char *path, char *first_part, const char **rest)
{
	while (*path == '/') path++;
	if (*path == '\0') {
		first_part[0] = '\0';
		*rest = 0;
		return;
	}
	usize i = 0;
	while (path[i] != '\0' && path[i] != '/') {
		first_part[i] = path[i];
		i++;
	}
	first_part[i] = '\0';
	*rest = path + i;
}

static struct vfs_node *find_child(struct vfs_node *parent, const char *name)
{
	struct vfs_node *child = parent->first_child;
	while (child) {
		if (strcmp(child->name, name) == 0) {
			return child;
		}
		child = child->next_sibling;
	}
	return 0;
}

struct vfs_node *vfs_find_node(const char *path)
{
	if (!root_node) return 0;
	if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
		return root_node;
	}

	struct vfs_node *current = root_node;
	char part[64];
	const char *rest = path;

	while (1) {
		split_path(rest, part, &rest);
		if (part[0] == '\0') break;

		current = find_child(current, part);
		if (!current) return 0;
	}

	return current;
}

static struct vfs_node *add_node(const char *path, enum vfs_node_type type, void *data, usize size, u32 flags)
{
	if (!root_node) {
		root_node = alloc_node();
		root_node->name[0] = '/';
		root_node->name[1] = '\0';
		root_node->type = VFS_DIRECTORY;
	}

	char part[64];
	const char *rest = path;
	struct vfs_node *current = root_node;

	while (1) {
		split_path(rest, part, &rest);
		if (part[0] == '\0') return current; // Path already exists or is root

		// If this is the last part
		if (!rest || rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0')) {
			struct vfs_node *child = find_child(current, part);
			if (!child) {
				child = alloc_node();
				if (!child) return 0;
				usize len = strlen(part);
				if (len > 63) len = 63;
				memcpy(child->name, part, len);
				child->name[len] = '\0';
				child->type = type;
				child->data = data;
				child->size = size;
				child->flags = flags;
				child->parent = current;
				child->next_sibling = current->first_child;
				current->first_child = child;
			}
			return child;
		} else {
			// Intermediate part must be a directory
			struct vfs_node *child = find_child(current, part);
			if (!child) {
				child = alloc_node();
				if (!child) return 0;
				usize len = strlen(part);
				if (len > 63) len = 63;
				memcpy(child->name, part, len);
				child->name[len] = '\0';
				child->type = VFS_DIRECTORY;
				child->parent = current;
				child->next_sibling = current->first_child;
				current->first_child = child;
			}
			current = child;
		}
	}
	return 0;
}

struct vfs_node *vfs_add_node(const char *path, enum vfs_node_type type, void *data, usize size, u32 flags)
{
	return add_node(path, type, data, size, flags);
}

static int alloc_handle(struct vfs_node *node)
{
	for (usize i = 0; i < MAX_VFS_HANDLES; i++) {
		if (!handles[i].used) {
			handles[i].used = 1;
			handles[i].node = node;
			handles[i].offset = 0;
			return (int)i;
		}
	}
	return -1;
}

void vfs_init(void)
{
	node_count = 0;
	memset(handles, 0, sizeof(handles));
	root_node = 0;

	// This triggers root creation
	add_node("/", VFS_DIRECTORY, 0, 0, 0);

	for (usize i = 0; i < initramfs_count(); i++) {
		const struct initramfs_file *file = initramfs_get(i);
		add_node(file->path, VFS_FILE, (void *)file->data, file->size, file->flags);
	}

	add_node("/dev/console", VFS_DEVICE, 0, 0, 0);
	add_node("/dev/virtio-blk0", VFS_DEVICE, 0, 0, 0);
	vfs_create("/tmp/hello", "tmpfs says hello\n");
#ifndef __aarch64__
	virtio_blk_init();

	struct block_device *blk = blk_get("virtio-blk0");
	if (blk) {
		fat32_mount(blk, "/mnt");
	}
#endif

	console_write("vfs: nodes 0x");
	console_write_hex64(node_count);
	console_write(" mounted as tree\n");
}

int vfs_open(const char *path)
{
	struct vfs_node *node = vfs_find_node(path);
	if (node == 0) return -1;

	/* Permission check: read access for opening */
	const struct cred *cred = get_current_cred();
	if (cred && !vfs_get_node_perm(node, cred, 0)) {
		console_write("vfs: permission denied opening ");
		console_write(path);
		console_write("\n");
		return -1;
	}

	return alloc_handle(node);
}

isize vfs_read(int handle, char *buffer, usize size)
{
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used) return -1;

	struct vfs_handle *h = &handles[handle];
	struct vfs_node *node = h->node;

	/* Permission check: read access */
	const struct cred *cred = get_current_cred();
	if (cred && !vfs_get_node_perm(node, cred, 0)) {
		return -1;
	}

	if (node->read_cb) {
		isize bytes = node->read_cb(node, h->offset, buffer, size);
		if (bytes > 0) {
			h->offset += bytes;
		}
		return bytes;
	}

	if (node->type == VFS_DEVICE || node->type == VFS_DIRECTORY) return 0;

	usize remaining = node->size > h->offset ? node->size - h->offset : 0;
	usize to_read = size < remaining ? size : remaining;

	if (to_read > 0) {
		memcpy(buffer, (const char *)node->data + h->offset, to_read);
		h->offset += to_read;
	}
	return (isize)to_read;
}

isize vfs_write(int handle, const char *buffer, usize size)
{
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used) return -1;

	struct vfs_node *node = handles[handle].node;
	struct vfs_handle *h = &handles[handle];

	/* Permission check: write access */
	const struct cred *cred = get_current_cred();
	if (cred && !vfs_get_node_perm(node, cred, 1)) {
		return -1;
	}

	if (node->write_cb) {
		isize bytes = node->write_cb(node, h->offset, buffer, size);
		if (bytes > 0) {
			h->offset += bytes;
		}
		return bytes;
	}

	if (node->type == VFS_DEVICE && strcmp(node->name, "console") == 0) {
		for (usize i = 0; i < size; i++) {
			console_putc(buffer[i]);
		}
		return (isize)size;
	}

	return -1;
}

void vfs_close(int handle)
{
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES) return;
	handles[handle].used = 0;
	handles[handle].node = 0;
	handles[handle].offset = 0;
}

int vfs_create(const char *path, const char *data)
{
	if (vfs_find_node(path) != 0) return -1;

	usize len = strlen(path);
	isize last_slash = -1;
	for (isize i = len - 1; i >= 0; i--) {
		if (path[i] == '/') {
			last_slash = i;
			break;
		}
	}

	if (last_slash >= 0) {
		char parent_path[256];
		if (last_slash == 0) {
			parent_path[0] = '/';
			parent_path[1] = '\0';
		} else {
			usize cp_len = last_slash < 255 ? last_slash : 255;
			memcpy(parent_path, path, cp_len);
			parent_path[cp_len] = '\0';
		}
		
		struct vfs_node *parent = vfs_find_node(parent_path);
		if (parent && parent->create_cb) {
			const char *name = path + last_slash + 1;
			if (parent->create_cb(parent, name, path) == 0) {
				return 0;
			}
			return -1;
		}

		/* Check write permission on parent directory */
		const struct cred *cred = get_current_cred();
		if (cred && parent && !vfs_get_node_perm(parent, cred, 1)) {
			console_write("vfs: permission denied creating ");
			console_write(path);
			console_write("\n");
			return -1;
		}
	}

	usize size = strlen(data);
	char *data_copy = kmalloc(size + 1);
	memcpy(data_copy, data, size + 1);

	return add_node(path, VFS_FILE, data_copy, size, 0) == 0 ? -1 : 0;
}

struct vfs_node *vfs_find_node_by_fd(int fd)
{
	if (fd < 0 || (usize)fd >= MAX_VFS_HANDLES || !handles[fd].used) return 0;
	return handles[fd].node;
}

int vfs_mkdir(const char *path)
{
	if (vfs_find_node(path) != 0) return -1;

	/* Permission check on parent */
	usize len = strlen(path);
	isize last_slash = -1;
	for (isize i = len - 1; i >= 0; i--) {
		if (path[i] == '/') {
			last_slash = i;
			break;
		}
	}

	if (last_slash >= 0) {
		char parent_path[256];
		if (last_slash == 0) {
			parent_path[0] = '/';
			parent_path[1] = '\0';
		} else {
			usize cp_len = last_slash < 255 ? last_slash : 255;
			memcpy(parent_path, path, cp_len);
			parent_path[cp_len] = '\0';
		}
		struct vfs_node *parent = vfs_find_node(parent_path);
		const struct cred *cred = get_current_cred();
		if (cred && parent && !vfs_get_node_perm(parent, cred, 1)) {
			console_write("vfs: permission denied mkdir ");
			console_write(path);
			console_write("\n");
			return -1;
		}
	}

	return add_node(path, VFS_DIRECTORY, 0, 0, 0) == 0 ? -1 : 0;
}

usize vfs_list(const char *dir_path, const char **names, usize max_names)
{
	struct vfs_node *dir = vfs_find_node(dir_path);
	if (!dir || dir->type != VFS_DIRECTORY) return 0;

	/* List requires read access on directory */
	const struct cred *cred = get_current_cred();
	if (cred && !vfs_get_node_perm(dir, cred, 0)) {
		return 0;
	}

	if (dir->list_cb) {
		return dir->list_cb(dir, names, max_names);
	}

	usize count = 0;
	struct vfs_node *child = dir->first_child;
	while (child && count < max_names) {
		names[count++] = child->name;
		child = child->next_sibling;
	}

	return count;
}

/* ── Permission Management Functions ── */

int vfs_chmod(const char *path, u16 mode)
{
	struct vfs_node *node = vfs_find_node(path);
	if (!node) return -1;

	const struct cred *cred = get_current_cred();
	if (!cred) return -1;

	/* Only owner or root can chmod */
	if (cred->euid != ROOT_UID && cred->euid != node->uid) {
		if (!cred_has_cap(cred, CAP_FOWNER)) return -1;
	}

	node->mode = (node->mode & ~0777) | (mode & 0777);
	return 0;
}

int vfs_chown(const char *path, u16 uid, u16 gid)
{
	struct vfs_node *node = vfs_find_node(path);
	if (!node) return -1;

	const struct cred *cred = get_current_cred();
	if (!cred) return -1;

	/* Only root can change owner */
	if (cred->euid != ROOT_UID && !cred_has_cap(cred, CAP_CHOWN)) return -1;

	if (uid != (u16)-1) node->uid = uid;
	if (gid != (u16)-1) node->gid = gid;
	return 0;
}

int vfs_set_acl(struct vfs_node *node, const struct acl_entry *acl)
{
	if (!node || !acl) return -1;

	const struct cred *cred = get_current_cred();
	if (!cred) return -1;

	/* Only owner or root can set ACLs */
	if (cred->euid != ROOT_UID && cred->euid != node->uid) {
		if (!cred_has_cap(cred, CAP_FOWNER)) return -1;
	}

	if (node->acl_count >= ACL_MAX_ENTRIES) return -1;
	node->acls[node->acl_count++] = *acl;
	return 0;
}

int vfs_get_acl(struct vfs_node *node, struct acl_entry *out_acl, int max_entries)
{
	if (!node || !out_acl) return -1;
	int count = node->acl_count < max_entries ? node->acl_count : max_entries;
	for (int i = 0; i < count; i++) {
		out_acl[i] = node->acls[i];
	}
	return count;
}
