#include <string.h>
#include <b1nix/console.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/net.h>
#include <b1nix/blk.h>
#include <b1nix/fat32.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>

#define MAX_VFS_NODES 256
#define MAX_VFS_HANDLES 64
#define MAX_VFS_PIPES 16
#define MAX_MOUNTS 16
#define PIPE_BUFFER_SIZE 512
#define TTY_INPUT_SIZE 256

static u16 bswap16(u16 v) { return (u16)((v << 8) | (v >> 8)); }

enum vfs_handle_kind {
	VFS_HANDLE_NONE = 0,
	VFS_HANDLE_NODE,
	VFS_HANDLE_PIPE_READ,
	VFS_HANDLE_PIPE_WRITE,
	VFS_HANDLE_SOCKET,
};

struct vfs_pipe {
	int used;
	char buffer[PIPE_BUFFER_SIZE];
	usize read_pos;
	usize write_pos;
	usize size;
	int readers;
	int writers;
};

struct vfs_socket_state {
	int domain;
	int type;
	int protocol;
	int bound;
	int connected;
	struct b1nix_sockaddr_in local;
	struct b1nix_sockaddr_in peer;
	/* Receive buffer for UDP */
	u8 recv_buf[2048];
	usize recv_len;
	/* TCP connection pointer (opaque) */
	void *tcp_conn;
};

struct vfs_handle {
	int used;
	int refcount;
	enum vfs_handle_kind kind;
	struct vfs_node *node;
	usize offset;
	struct vfs_pipe *pipe;
	struct vfs_socket_state socket;
	int flags;
};

static struct vfs_node nodes[MAX_VFS_NODES];
static struct vfs_handle handles[MAX_VFS_HANDLES];
static struct vfs_pipe pipes[MAX_VFS_PIPES];
struct vfs_mount_entry {
	int used;
	char source[VFS_MAX_PATH];
	char target[VFS_MAX_PATH];
	char fstype[16];
	u64 flags;
};
static struct vfs_mount_entry mounts[MAX_MOUNTS];
static usize node_count = 0;
static struct vfs_node *root_node = 0;
static struct b1nix_termios tty_termios;
static char tty_line[TTY_INPUT_SIZE];
static usize tty_line_pos;
static usize tty_line_len;

void virtio_blk_init(void);
#ifndef __aarch64__
extern char ps2_kbd_getc(void);
#endif

int vfs_mount(const char *source, const char *target, const char *fstype, u64 flags);

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
				if (flags & INITRAMFS_EXECUTABLE) {
					child->mode |= VFS_IXUSR | VFS_IXGRP | VFS_IXOTH;
				}
				child->parent = current;
				child->next_sibling = current->first_child;
				current->first_child = child;
			} else if (data != 0 || size != 0 || flags != 0 || type == VFS_DIRECTORY) {
				child->type = type;
				child->data = data;
				child->size = size;
				child->flags = flags;
				if (flags & INITRAMFS_EXECUTABLE) {
					child->mode |= VFS_IXUSR | VFS_IXGRP | VFS_IXOTH;
				}
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
			handles[i].refcount = 1;
			handles[i].kind = VFS_HANDLE_NODE;
			handles[i].node = node;
			handles[i].offset = 0;
			handles[i].pipe = 0;
			handles[i].flags = 0;
			return (int)i;
		}
	}
	return -1;
}

static int alloc_raw_handle(enum vfs_handle_kind kind)
{
	for (usize i = 0; i < MAX_VFS_HANDLES; i++) {
		if (!handles[i].used) {
			memset(&handles[i], 0, sizeof(handles[i]));
			handles[i].used = 1;
			handles[i].refcount = 1;
			handles[i].kind = kind;
			return (int)i;
		}
	}
	return -1;
}

void vfs_handle_retain(int handle)
{
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used) return;
	handles[handle].refcount++;
}

static struct vfs_handle *get_handle(int fd)
{
	int handle = scheduler_fd_get(fd);
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES || !handles[handle].used) return 0;
	return &handles[handle];
}

static void copy_path(char *dst, usize dst_size, const char *src)
{
	if (!dst || dst_size == 0) return;
	if (!src) src = "";
	usize len = strlen(src);
	if (len >= dst_size) len = dst_size - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void release_handle(int handle)
{
	if (handle < 0 || (usize)handle >= MAX_VFS_HANDLES) return;
	if (!handles[handle].used) return;
	if (handles[handle].refcount > 1) {
		handles[handle].refcount--;
		return;
	}

	if (handles[handle].kind == VFS_HANDLE_PIPE_READ && handles[handle].pipe) {
		handles[handle].pipe->readers--;
		if (handles[handle].pipe->readers <= 0 && handles[handle].pipe->writers <= 0) {
			handles[handle].pipe->used = 0;
		}
	} else if (handles[handle].kind == VFS_HANDLE_PIPE_WRITE && handles[handle].pipe) {
		handles[handle].pipe->writers--;
		if (handles[handle].pipe->readers <= 0 && handles[handle].pipe->writers <= 0) {
			handles[handle].pipe->used = 0;
		}
	}
	handles[handle].used = 0;
	handles[handle].refcount = 0;
	handles[handle].kind = VFS_HANDLE_NONE;
	handles[handle].node = 0;
	handles[handle].offset = 0;
	handles[handle].pipe = 0;
	handles[handle].flags = 0;
}

void vfs_handle_release(int handle)
{
	release_handle(handle);
}

static char tty_getc_blocking(void)
{
#ifdef __aarch64__
	return 0;
#else
	char c = 0;
	while (c == 0) {
		c = ps2_kbd_getc();
		if (c == 0) scheduler_yield();
	}
	return c;
#endif
}

static isize tty_read(struct vfs_node *node, u64 offset, char *buffer, usize size)
{
	(void)node;
	(void)offset;
	if (!buffer || size == 0) return 0;

	if ((tty_termios.c_lflag & B1NIX_ICANON) == 0) {
		for (usize i = 0; i < size; i++) {
			buffer[i] = tty_getc_blocking();
		}
		return (isize)size;
	}

	while (tty_line_pos >= tty_line_len) {
		tty_line_pos = 0;
		tty_line_len = 0;

		while (tty_line_len < sizeof(tty_line) - 1) {
			char c = tty_getc_blocking();
			if (c == 0) return 0;
			if (c == 27) {
				char next = tty_getc_blocking();
				if (next == '[') {
					(void)tty_getc_blocking();
					continue;
				}
				continue;
			}
			if ((tty_termios.c_lflag & B1NIX_ISIG) && c == 3) {
				console_write("^C\n");
				tty_line_len = 0;
				tty_line[tty_line_len++] = '\n';
				break;
			}
			if ((tty_termios.c_lflag & B1NIX_ISIG) && c == 26) {
				console_write("^Z\n");
				tty_line_len = 0;
				tty_line[tty_line_len++] = '\n';
				break;
			}
			if (c == 4) {
				break;
			}
			if (c == '\b' || c == 127) {
				if (tty_line_len > 0) {
					tty_line_len--;
					if (tty_termios.c_lflag & B1NIX_ECHO) {
						console_write("\b \b");
					}
				}
				continue;
			}
			tty_line[tty_line_len++] = c;
			if (tty_termios.c_lflag & B1NIX_ECHO) {
				console_putc(c);
			}
			if (c == '\n') break;
		}
	}

	usize copied = 0;
	while (copied < size && tty_line_pos < tty_line_len) {
		buffer[copied++] = tty_line[tty_line_pos++];
	}
	return (isize)copied;
}

static isize tty_write(struct vfs_node *node, u64 offset, const char *buffer, usize size)
{
	(void)node;
	(void)offset;
	if (!buffer) return -1;
	for (usize i = 0; i < size; i++) {
		if ((tty_termios.c_oflag & B1NIX_OPOST) && buffer[i] == '\n') {
			console_putc('\r');
		}
		console_putc(buffer[i]);
	}
	return (isize)size;
}

static void tty_init_node(void)
{
	memset(&tty_termios, 0, sizeof(tty_termios));
	tty_termios.c_lflag = B1NIX_ICANON | B1NIX_ECHO | B1NIX_ISIG;
	tty_termios.c_oflag = B1NIX_OPOST;
	struct vfs_node *tty = add_node("/dev/tty", VFS_DEVICE, 0, 0, 0);
	if (tty) {
		tty->read_cb = tty_read;
		tty->write_cb = tty_write;
		tty->mode = VFS_IRUSR | VFS_IWUSR | VFS_IRGRP | VFS_IWGRP | VFS_IROTH | VFS_IWOTH;
	}
}

static void vfs_init_stdio(void)
{
	scheduler_fd_table_init_current();
	int tty = vfs_open("/dev/tty");
	if (tty < 0) return;
	vfs_dup2(tty, 0);
	vfs_dup2(tty, 1);
	vfs_dup2(tty, 2);
}

void vfs_init(void)
{
	node_count = 0;
	memset(handles, 0, sizeof(handles));
	memset(pipes, 0, sizeof(pipes));
	memset(mounts, 0, sizeof(mounts));
	root_node = 0;

	// This triggers root creation
	add_node("/", VFS_DIRECTORY, 0, 0, 0);

	for (usize i = 0; i < initramfs_count(); i++) {
		const struct initramfs_file *file = initramfs_get(i);
		add_node(file->path, VFS_FILE, (void *)file->data, file->size, file->flags);
	}

	add_node("/dev/console", VFS_DEVICE, 0, 0, 0);
	tty_init_node();
	add_node("/dev/virtio-blk0", VFS_DEVICE, 0, 0, 0);
	vfs_mkdir("/home");
	vfs_mkdir("/tmp");
	vfs_mkdir("/var");
	vfs_mkdir("/mnt");
	vfs_mkdir("/proc");
	vfs_create("/tmp/hello", "tmpfs says hello\n");
	vfs_mount("initramfs", "/", "initramfs", 0);
	vfs_init_stdio();
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
	return vfs_open_flags(path, B1NIX_O_RDONLY);
}

int vfs_open_flags(const char *path, int flags)
{
	struct vfs_node *node = vfs_find_node(path);
	if (node == 0 && (flags & B1NIX_O_CREAT)) {
		if (vfs_create(path, "") != 0) return -1;
		node = vfs_find_node(path);
	}
	if (node == 0) return -1;
	if ((flags & B1NIX_O_DIRECTORY) && node->type != VFS_DIRECTORY) return -1;

	/* Permission check: read access for opening */
	const struct cred *cred = get_current_cred();
	int write_access = (flags & (B1NIX_O_WRONLY | B1NIX_O_RDWR | B1NIX_O_TRUNC)) != 0;
	if (cred && !vfs_get_node_perm(node, cred, write_access)) {
		console_write("vfs: permission denied opening ");
		console_write(path);
		console_write("\n");
		return -1;
	}

	if ((flags & B1NIX_O_TRUNC) && node->type == VFS_FILE) {
		node->size = 0;
		if (node->data) ((char *)node->data)[0] = '\0';
	}

	int handle = alloc_handle(node);
	if (handle < 0) return -1;
	handles[handle].flags = flags;
	if (flags & B1NIX_O_APPEND) {
		handles[handle].offset = node->size;
	}
	int fd = scheduler_fd_alloc(handle);
	if (fd < 0) {
		release_handle(handle);
		return -1;
	}
	return fd;
}

isize vfs_read(int handle, char *buffer, usize size)
{
	struct vfs_handle *h = get_handle(handle);
	if (!h) return -1;
	if (h->kind == VFS_HANDLE_PIPE_READ) {
		struct vfs_pipe *pipe = h->pipe;
		if (!pipe || !pipe->used) return -1;
		usize to_read = size < pipe->size ? size : pipe->size;
		for (usize i = 0; i < to_read; i++) {
			buffer[i] = pipe->buffer[pipe->read_pos];
			pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUFFER_SIZE;
		}
		pipe->size -= to_read;
		return (isize)to_read;
	}

	if (h->kind == VFS_HANDLE_SOCKET) {
		return vfs_socket_recv(handle, buffer, size, 0);
	}

	if (h->kind != VFS_HANDLE_NODE) return -1;
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
	struct vfs_handle *h = get_handle(handle);
	if (!h) return -1;
	if (h->kind == VFS_HANDLE_PIPE_WRITE) {
		struct vfs_pipe *pipe = h->pipe;
		if (!pipe || !pipe->used) return -1;
		usize written = 0;
		while (written < size && pipe->size < PIPE_BUFFER_SIZE) {
			pipe->buffer[pipe->write_pos] = buffer[written++];
			pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUFFER_SIZE;
			pipe->size++;
		}
		return (isize)written;
	}

	if (h->kind == VFS_HANDLE_SOCKET) {
		return vfs_socket_send(handle, buffer, size, 0);
	}

	if (h->kind != VFS_HANDLE_NODE) return -1;
	struct vfs_node *node = h->node;

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

	if (node->type == VFS_FILE) {
		usize needed = h->offset + size;
		if (needed > node->size) {
			char *new_data = kmalloc(needed + 1);
			if (!new_data) return -1;
			if (node->data && node->size > 0) memcpy(new_data, node->data, node->size);
			if (needed > node->size) memset(new_data + node->size, 0, needed - node->size);
			node->data = new_data;
			node->size = needed;
		}
		memcpy((char *)node->data + h->offset, buffer, size);
		h->offset += size;
		((char *)node->data)[node->size] = '\0';
		return (isize)size;
	}

	return -1;
}

void vfs_close(int handle)
{
	int raw = scheduler_fd_get(handle);
	if (raw < 0) return;

	/* Clean up TCP connection if socket */
	if (raw < MAX_VFS_HANDLES && handles[raw].used &&
	    handles[raw].kind == VFS_HANDLE_SOCKET &&
	    handles[raw].socket.type == B1NIX_SOCK_STREAM &&
	    handles[raw].socket.tcp_conn) {
		tcp_close((struct tcp_conn *)handles[raw].socket.tcp_conn);
		handles[raw].socket.tcp_conn = 0;
	}

	scheduler_fd_close(handle);
	release_handle(raw);
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
	struct vfs_handle *h = get_handle(fd);
	if (!h || h->kind != VFS_HANDLE_NODE) return 0;
	return h->node;
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

int vfs_stat(const char *path, struct b1nix_stat *st)
{
	if (!path || !st) return -1;
	struct vfs_node *node = vfs_find_node(path);
	if (!node) return -1;

	memset(st, 0, sizeof(*st));
	st->st_ino = (u64)(usize)(node - nodes + 1);
	st->st_uid = node->uid;
	st->st_gid = node->gid;
	st->st_size = node->size;
	st->st_blksize = 512;
	st->st_blocks = (node->size + 511) / 512;
	st->st_nlink = (node->type == VFS_DIRECTORY) ? 2 : 1;

	u32 type = B1NIX_S_IFREG;
	if (node->type == VFS_DIRECTORY) type = B1NIX_S_IFDIR;
	if (node->type == VFS_DEVICE) type = B1NIX_S_IFCHR;
	st->st_mode = type | (node->mode & 0777);
	return 0;
}

isize vfs_lseek(int handle, isize offset, int whence)
{
	struct vfs_handle *h = get_handle(handle);
	if (!h || h->kind != VFS_HANDLE_NODE) return -1;
	isize base = 0;
	if (whence == B1NIX_SEEK_SET) {
		base = 0;
	} else if (whence == B1NIX_SEEK_CUR) {
		base = (isize)h->offset;
	} else if (whence == B1NIX_SEEK_END) {
		base = h->node ? (isize)h->node->size : 0;
	} else {
		return -1;
	}

	isize next = base + offset;
	if (next < 0) return -1;
	h->offset = (usize)next;
	return next;
}

static int split_parent_path(const char *path, char *parent_path, char *name)
{
	if (!path || path[0] == '\0') return -1;
	usize len = strlen(path);
	if (len == 0 || len >= 256) return -1;
	isize last_slash = -1;
	for (isize i = (isize)len - 1; i >= 0; i--) {
		if (path[i] == '/') {
			last_slash = i;
			break;
		}
	}

	if (last_slash < 0) {
		parent_path[0] = '/';
		parent_path[1] = '\0';
		memcpy(name, path, len + 1);
		return 0;
	}

	if ((usize)last_slash == len - 1) return -1;
	if (last_slash == 0) {
		parent_path[0] = '/';
		parent_path[1] = '\0';
	} else {
		memcpy(parent_path, path, (usize)last_slash);
		parent_path[last_slash] = '\0';
	}
	memcpy(name, path + last_slash + 1, len - (usize)last_slash);
	return 0;
}

int vfs_unlink(const char *path)
{
	char parent_path[256];
	char name[64];
	if (split_parent_path(path, parent_path, name) < 0) return -1;

	struct vfs_node *parent = vfs_find_node(parent_path);
	if (!parent || parent->type != VFS_DIRECTORY) return -1;

	const struct cred *cred = get_current_cred();
	if (cred && !vfs_get_node_perm(parent, cred, 1)) return -1;

	struct vfs_node *prev = 0;
	struct vfs_node *child = parent->first_child;
	while (child) {
		if (strcmp(child->name, name) == 0) {
			if (child->type == VFS_DIRECTORY && child->first_child) return -1;
			if (prev) prev->next_sibling = child->next_sibling;
			else parent->first_child = child->next_sibling;
			child->parent = 0;
			child->next_sibling = 0;
			child->type = 0;
			return 0;
		}
		prev = child;
		child = child->next_sibling;
	}
	return -1;
}

int vfs_rename(const char *old_path, const char *new_path)
{
	char old_parent_path[256];
	char old_name[64];
	char new_parent_path[256];
	char new_name[64];
	if (split_parent_path(old_path, old_parent_path, old_name) < 0) return -1;
	if (split_parent_path(new_path, new_parent_path, new_name) < 0) return -1;

	struct vfs_node *old_parent = vfs_find_node(old_parent_path);
	struct vfs_node *new_parent = vfs_find_node(new_parent_path);
	if (!old_parent || !new_parent || new_parent->type != VFS_DIRECTORY) return -1;
	if (vfs_find_node(new_path)) return -1;

	struct vfs_node *node = find_child(old_parent, old_name);
	if (!node) return -1;
	const struct cred *cred = get_current_cred();
	if (cred && (!vfs_get_node_perm(old_parent, cred, 1) || !vfs_get_node_perm(new_parent, cred, 1))) return -1;

	struct vfs_node *prev = 0;
	struct vfs_node *child = old_parent->first_child;
	while (child) {
		if (child == node) {
			if (prev) prev->next_sibling = child->next_sibling;
			else old_parent->first_child = child->next_sibling;
			break;
		}
		prev = child;
		child = child->next_sibling;
	}

	copy_path(node->name, sizeof(node->name), new_name);
	node->parent = new_parent;
	node->next_sibling = new_parent->first_child;
	new_parent->first_child = node;
	return 0;
}

int vfs_rmdir(const char *path)
{
	struct vfs_node *node = vfs_find_node(path);
	if (!node || node->type != VFS_DIRECTORY || node->first_child) return -1;
	return vfs_unlink(path);
}

int vfs_fstat(int fd, struct b1nix_stat *st)
{
	struct vfs_node *node = vfs_find_node_by_fd(fd);
	if (!node || !st) return -1;
	memset(st, 0, sizeof(*st));
	st->st_ino = (u64)(usize)(node - nodes + 1);
	st->st_uid = node->uid;
	st->st_gid = node->gid;
	st->st_size = node->size;
	st->st_blksize = 512;
	st->st_blocks = (node->size + 511) / 512;
	st->st_nlink = (node->type == VFS_DIRECTORY) ? 2 : 1;
	u32 type = B1NIX_S_IFREG;
	if (node->type == VFS_DIRECTORY) type = B1NIX_S_IFDIR;
	if (node->type == VFS_DEVICE) type = B1NIX_S_IFCHR;
	st->st_mode = type | (node->mode & 0777);
	return 0;
}

int vfs_fsync(int fd)
{
	if (scheduler_fd_get(fd) < 0) return -1;
	blk_cache_flush(0);
	return 0;
}

int vfs_mount(const char *source, const char *target, const char *fstype, u64 flags)
{
	if (!target || target[0] == '\0') return -1;
	struct vfs_node *target_node = vfs_find_node(target);
	if (!target_node) {
		if (vfs_mkdir(target) != 0) return -1;
		target_node = vfs_find_node(target);
	}
	if (!target_node || target_node->type != VFS_DIRECTORY) return -1;

	for (usize i = 0; i < MAX_MOUNTS; i++) {
		if (mounts[i].used && strcmp(mounts[i].target, target) == 0) {
			copy_path(mounts[i].source, sizeof(mounts[i].source), source);
			copy_path(mounts[i].fstype, sizeof(mounts[i].fstype), fstype);
			mounts[i].flags = flags;
			return 0;
		}
	}
	for (usize i = 0; i < MAX_MOUNTS; i++) {
		if (!mounts[i].used) {
			mounts[i].used = 1;
			copy_path(mounts[i].source, sizeof(mounts[i].source), source);
			copy_path(mounts[i].target, sizeof(mounts[i].target), target);
			copy_path(mounts[i].fstype, sizeof(mounts[i].fstype), fstype);
			mounts[i].flags = flags;
			return 0;
		}
	}
	return -1;
}

int vfs_umount(const char *target)
{
	if (!target) return -1;
	for (usize i = 0; i < MAX_MOUNTS; i++) {
		if (mounts[i].used && strcmp(mounts[i].target, target) == 0) {
			if (strcmp(target, "/") == 0) return -1;
			mounts[i].used = 0;
			return 0;
		}
	}
	return -1;
}

int vfs_sync(void)
{
	blk_cache_flush(0);
	return 0;
}

isize vfs_getdents(int handle, struct dirent *buf, usize max_entries)
{
	struct vfs_handle *h = get_handle(handle);
	if (!h || h->kind != VFS_HANDLE_NODE) return -1;
	struct vfs_node *dir = h->node;
	if (!dir || dir->type != VFS_DIRECTORY || !buf) return -1;

	usize skipped = 0;
	usize count = 0;
	struct vfs_node *child = dir->first_child;
	while (child && count < max_entries) {
		if (skipped++ < h->offset) {
			child = child->next_sibling;
			continue;
		}

		usize len = strlen(child->name);
		if (len > 63) len = 63;
		memcpy(buf[count].name, child->name, len);
		buf[count].name[len] = '\0';
		buf[count].type = (u32)child->type;
		buf[count].is_dir = child->type == VFS_DIRECTORY;
		buf[count].is_exec = (child->mode & 0111) ? 1 : 0;
		buf[count].size = child->size;
		count++;
		child = child->next_sibling;
	}
	h->offset += count;
	return (isize)count;
}

int vfs_pipe(int pipefd[2])
{
	if (!pipefd) return -1;
	struct vfs_pipe *pipe = 0;
	for (usize i = 0; i < MAX_VFS_PIPES; i++) {
		if (!pipes[i].used) {
			pipe = &pipes[i];
			break;
		}
	}
	if (!pipe) return -1;

	int rfd = alloc_raw_handle(VFS_HANDLE_PIPE_READ);
	if (rfd < 0) return -1;
	int wfd = alloc_raw_handle(VFS_HANDLE_PIPE_WRITE);
	if (wfd < 0) {
		release_handle(rfd);
		return -1;
	}

	memset(pipe, 0, sizeof(*pipe));
	pipe->used = 1;
	pipe->readers = 1;
	pipe->writers = 1;
	handles[rfd].pipe = pipe;
	handles[wfd].pipe = pipe;
	pipefd[0] = scheduler_fd_alloc(rfd);
	pipefd[1] = scheduler_fd_alloc(wfd);
	if (pipefd[0] < 0 || pipefd[1] < 0) {
		if (pipefd[0] >= 0) scheduler_fd_close(pipefd[0]);
		if (pipefd[1] >= 0) scheduler_fd_close(pipefd[1]);
		release_handle(rfd);
		release_handle(wfd);
		return -1;
	}
	return 0;
}

int vfs_dup2(int oldfd, int newfd)
{
	int old_handle = scheduler_fd_get(oldfd);
	if (old_handle < 0 || (usize)old_handle >= MAX_VFS_HANDLES || !handles[old_handle].used) return -1;
	if (newfd < 0 || (usize)newfd >= SCHED_MAX_FDS) return -1;
	if (oldfd == newfd) return newfd;

	if (scheduler_fd_get(newfd) >= 0) vfs_close(newfd);
	scheduler_fd_set(newfd, old_handle);
	vfs_handle_retain(old_handle);
	return newfd;
}

int vfs_fcntl(int fd, int cmd, u64 arg)
{
	struct vfs_handle *h = get_handle(fd);
	if (!h) return -1;
	switch (cmd) {
	case B1NIX_F_GETFD:
		return scheduler_fd_flags_get(fd);
	case B1NIX_F_SETFD:
		return scheduler_fd_flags_set(fd, (int)arg);
	case B1NIX_F_GETFL:
		return h->flags;
	case B1NIX_F_SETFL:
		h->flags = (int)arg;
		return 0;
	default:
		return -1;
	}
}

int vfs_ioctl(int fd, u64 request, void *arg)
{
	struct vfs_node *node = vfs_find_node_by_fd(fd);
	if (!node || node->type != VFS_DEVICE || !arg) return -1;
	if (strcmp(node->name, "tty") != 0 && strcmp(node->name, "console") != 0) return -1;

	if (request == B1NIX_TCGETS) {
		*(struct b1nix_termios *)arg = tty_termios;
		return 0;
	}
	if (request == B1NIX_TCSETS) {
		tty_termios = *(const struct b1nix_termios *)arg;
		return 0;
	}
	return -1;
}

void vfs_close_on_exec(void)
{
	for (int fd = 0; fd < SCHED_MAX_FDS; fd++) {
		int flags = scheduler_fd_flags_get(fd);
		if (flags >= 0 && (flags & B1NIX_FD_CLOEXEC) != 0) {
			vfs_close(fd);
		}
	}
}

int vfs_socket(int domain, int type, int protocol)
{
	if (domain != B1NIX_AF_INET) return -1;
	if (type != B1NIX_SOCK_DGRAM && type != B1NIX_SOCK_STREAM) return -1;
	int handle = alloc_raw_handle(VFS_HANDLE_SOCKET);
	if (handle < 0) return -1;
	handles[handle].socket.domain = domain;
	handles[handle].socket.type = type;
	handles[handle].socket.protocol = protocol;
	int fd = scheduler_fd_alloc(handle);
	if (fd < 0) {
		release_handle(handle);
		return -1;
	}
	return fd;
}

int vfs_bind(int fd, const void *addr, usize addrlen)
{
	struct vfs_handle *h = get_handle(fd);
	if (!h || h->kind != VFS_HANDLE_SOCKET) return -1;
	if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in)) return -1;
	h->socket.local = *(const struct b1nix_sockaddr_in *)addr;
	h->socket.bound = 1;
	return 0;
}

int vfs_connect(int fd, const void *addr, usize addrlen)
{
	struct vfs_handle *h = get_handle(fd);
	if (!h || h->kind != VFS_HANDLE_SOCKET) return -1;
	if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in)) return -1;
	h->socket.peer = *(const struct b1nix_sockaddr_in *)addr;
	h->socket.connected = 1;

	/* For TCP, initiate the real TCP connection */
	if (h->socket.type == B1NIX_SOCK_STREAM) {
		struct ipv4_addr dst;
		u32 raw_addr = h->socket.peer.sin_addr;
		dst.bytes[0] = (u8)(raw_addr & 0xFF);
		dst.bytes[1] = (u8)((raw_addr >> 8) & 0xFF);
		dst.bytes[2] = (u8)((raw_addr >> 16) & 0xFF);
		dst.bytes[3] = (u8)((raw_addr >> 24) & 0xFF);

		u16 port = bswap16(h->socket.peer.sin_port);

		struct tcp_conn *conn = tcp_connect(dst, port);
		if (!conn) {
			h->socket.connected = 0;
			return -1;
		}
		h->socket.tcp_conn = conn;
	}
	return 0;
}

isize vfs_socket_send(int fd, const void *buf, usize len, int flags)
{
	(void)flags;
	struct vfs_handle *h = get_handle(fd);
	if (!h || h->kind != VFS_HANDLE_SOCKET || !buf) return -1;
	if (!h->socket.connected && !h->socket.bound) return -1;

	if (h->socket.type == B1NIX_SOCK_DGRAM) {
		/* UDP send */
		struct ipv4_addr dst;
		u32 raw_addr = h->socket.peer.sin_addr;
		dst.bytes[0] = (u8)(raw_addr & 0xFF);
		dst.bytes[1] = (u8)((raw_addr >> 8) & 0xFF);
		dst.bytes[2] = (u8)((raw_addr >> 16) & 0xFF);
		dst.bytes[3] = (u8)((raw_addr >> 24) & 0xFF);

		u16 src_port = bswap16(h->socket.local.sin_port);
		u16 dst_port = bswap16(h->socket.peer.sin_port);

		udp_send(dst, src_port, dst_port, buf, len);
		return (isize)len;
	} else if (h->socket.type == B1NIX_SOCK_STREAM) {
		/* TCP send */
		if (!h->socket.tcp_conn) return -1;
		return (isize)tcp_send((struct tcp_conn *)h->socket.tcp_conn, buf, len);
	}
	return -1;
}

isize vfs_socket_recv(int fd, void *buf, usize len, int flags)
{
	(void)flags;
	struct vfs_handle *h = get_handle(fd);
	if (!h || h->kind != VFS_HANDLE_SOCKET || !buf) return -1;

	if (h->socket.type == B1NIX_SOCK_DGRAM) {
		/* UDP recv — check internal buffer */
		if (h->socket.recv_len == 0) {
			/* Poll network to try to get data */
			net_poll();
		}
		if (h->socket.recv_len == 0) return 0;

		usize copy = (len < h->socket.recv_len) ? len : h->socket.recv_len;
		memcpy(buf, h->socket.recv_buf, copy);
		h->socket.recv_len = 0;
		return (isize)copy;
	} else if (h->socket.type == B1NIX_SOCK_STREAM) {
		/* TCP recv */
		if (!h->socket.tcp_conn) return -1;
		return (isize)tcp_recv((struct tcp_conn *)h->socket.tcp_conn, buf, len);
	}

	if (len > 0) memset(buf, 0, len);
	return 0;
}

/* ── Push data into UDP socket receive buffer (called from udp_receive) ── */
void vfs_socket_push_udp(u16 local_port, const void *data, usize len)
{
	for (int i = 0; i < MAX_VFS_HANDLES; i++) {
		if (!handles[i].used || handles[i].kind != VFS_HANDLE_SOCKET) continue;
		if (handles[i].socket.type != B1NIX_SOCK_DGRAM) continue;
		if (!handles[i].socket.bound) continue;

		u16 sock_port = bswap16(handles[i].socket.local.sin_port);
		if (sock_port == local_port && handles[i].socket.recv_len == 0) {
			usize copy = (len > sizeof(handles[i].socket.recv_buf))
			             ? sizeof(handles[i].socket.recv_buf) : len;
			memcpy(handles[i].socket.recv_buf, data, copy);
			handles[i].socket.recv_len = copy;
			return;
		}
	}
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
