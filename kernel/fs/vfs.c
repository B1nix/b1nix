#include <string.h>
#include <b1nix/console.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/blk.h>
#include <b1nix/fat32.h>

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

static struct vfs_node *alloc_node(void)
{
	if (node_count >= MAX_VFS_NODES) {
		return 0;
	}
	struct vfs_node *node = &nodes[node_count++];
	memset(node, 0, sizeof(*node));
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
	virtio_blk_init();

	struct block_device *blk = blk_get("virtio-blk0");
	if (blk) {
		fat32_mount(blk, "/mnt");
	}

	console_write("vfs: nodes 0x");
	console_write_hex64(node_count);
	console_write(" mounted as tree\n");
}

int vfs_open(const char *path)
{
	struct vfs_node *node = vfs_find_node(path);
	if (node == 0) return -1;
	return alloc_handle(node);
}

isize vfs_read(int handle, char *buffer, usize size)
{
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used) return -1;

	struct vfs_handle *h = &handles[handle];
	struct vfs_node *node = h->node;

	if (node->type == VFS_DEVICE || node->type == VFS_DIRECTORY) return 0;

	usize remaining = node->size - h->offset;
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

	usize size = strlen(data);
	char *data_copy = kmalloc(size + 1);
	memcpy(data_copy, data, size + 1);

	return add_node(path, VFS_FILE, data_copy, size, 0) == 0 ? -1 : 0;
}

int vfs_mkdir(const char *path)
{
	if (vfs_find_node(path) != 0) return -1;
	return add_node(path, VFS_DIRECTORY, 0, 0, 0) == 0 ? -1 : 0;
}

usize vfs_list(const char *dir_path, const char **names, usize max_names)
{
	struct vfs_node *dir = vfs_find_node(dir_path);
	if (!dir || dir->type != VFS_DIRECTORY) return 0;

	usize count = 0;
	struct vfs_node *child = dir->first_child;
	while (child && count < max_names) {
		names[count++] = child->name;
		child = child->next_sibling;
	}

	return count;
}
