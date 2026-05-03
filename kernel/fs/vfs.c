#include <string.h>
#include <b1nix/console.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>

#define MAX_VFS_NODES 32
#define MAX_VFS_HANDLES 32

struct vfs_handle {
	int used;
	struct vfs_node *node;
	usize offset;
};

static struct vfs_node nodes[MAX_VFS_NODES];
static struct vfs_handle handles[MAX_VFS_HANDLES];
static usize node_count;

void virtio_blk_init(void);

static char *kstrdup(const char *text)
{
	usize size = strlen(text) + 1;
	char *copy = kmalloc(size);
	memcpy(copy, text, size);
	return copy;
}

static struct vfs_node *find_node(const char *path)
{
	for (usize i = 0; i < node_count; i++) {
		if (strcmp(nodes[i].path, path) == 0) {
			return &nodes[i];
		}
	}

	return 0;
}

static struct vfs_node *add_node(const char *path, enum vfs_node_type type, void *data, usize size, u32 flags)
{
	if (node_count >= MAX_VFS_NODES) {
		return 0;
	}

	struct vfs_node *node = &nodes[node_count++];
	node->path = path;
	node->type = type;
	node->data = data;
	node->size = size;
	node->flags = flags;
	return node;
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

	for (usize i = 0; i < initramfs_count(); i++) {
		const struct initramfs_file *file = initramfs_get(i);
		add_node(file->path, VFS_FILE, (void *)file->data, file->size, file->flags);
	}

	add_node("/dev/console", VFS_DEVICE, 0, 0, 0);
	add_node("/dev/virtio-blk0", VFS_DEVICE, 0, 0, 0);
	vfs_create("/tmp/hello", "tmpfs says hello\n");
	virtio_blk_init();

	console_write("vfs: nodes 0x");
	console_write_hex64(node_count);
	console_write("\n");
}

int vfs_open(const char *path)
{
	struct vfs_node *node = find_node(path);

	if (node == 0) {
		return -1;
	}

	return alloc_handle(node);
}

isize vfs_read(int handle, char *buffer, usize size)
{
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used) {
		return -1;
	}

	struct vfs_handle *h = &handles[handle];
	struct vfs_node *node = h->node;

	if (node->type == VFS_DEVICE) {
		return 0;
	}

	usize remaining = node->size - h->offset;
	usize to_read = size < remaining ? size : remaining;

	memcpy(buffer, (const char *)node->data + h->offset, to_read);
	h->offset += to_read;
	return (isize)to_read;
}

isize vfs_write(int handle, const char *buffer, usize size)
{
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used) {
		return -1;
	}

	struct vfs_node *node = handles[handle].node;

	if (strcmp(node->path, "/dev/console") == 0) {
		for (usize i = 0; i < size; i++) {
			console_putc(buffer[i]);
		}

		return (isize)size;
	}

	return -1;
}

void vfs_close(int handle)
{
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES) {
		return;
	}

	handles[handle].used = 0;
	handles[handle].node = 0;
	handles[handle].offset = 0;
}

int vfs_create(const char *path, const char *data)
{
	if (find_node(path) != 0) {
		return -1;
	}

	usize size = strlen(data);
	char *path_copy = kstrdup(path);
	char *data_copy = kmalloc(size + 1);
	memcpy(data_copy, data, size + 1);

	return add_node(path_copy, VFS_FILE, data_copy, size, 0) == 0 ? -1 : 0;
}

usize vfs_list(const char **paths, usize max_paths)
{
	usize count = node_count < max_paths ? node_count : max_paths;

	for (usize i = 0; i < count; i++) {
		paths[i] = nodes[i].path;
	}

	return count;
}
