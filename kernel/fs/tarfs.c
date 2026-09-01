/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tarfs.c — POSIX USTAR Archive Filesystem for b1nix.
 *
 * Mounts standard uncompressed .tar archives directly as a read-only VFS tree
 * from any block device (SATA, NVMe, VirtIO, RAM-disk, loop device).
 * Eliminates the need to format ext4/FAT disks when distributing or testing
 * pre-packaged userspace test suites and root filesystems.
 */

#include <b1nix/tarfs.h>
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

#define TAR_BLOCK_SIZE 512

struct tar_header {
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char chksum[8];
	char typeflag;
	char linkname[100];
	char magic[6];
	char version[2];
	char uname[32];
	char gname[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
	char pad[12];
};

struct tarfs_file_info {
	struct block_device *bdev;
	u64 start_lba;
	u64 size;
};

static u64 parse_octal(const char *p, usize len)
{
	u64 v = 0;
	while (len > 0 && (*p == ' ' || *p == '\0')) {
		p++;
		len--;
	}
	while (len > 0 && *p >= '0' && *p <= '7') {
		v = (v << 3) | (*p - '0');
		p++;
		len--;
	}
	return v;
}

static int tar_verify_checksum(const struct tar_header *h)
{
	const u8 *p = (const u8 *)h;
	u32 sum = 0;
	for (usize i = 0; i < sizeof(struct tar_header); i++) {
		if (i >= 148 && i < 156)
			sum += ' ';
		else
			sum += p[i];
	}
	u32 expected = (u32)parse_octal(h->chksum, sizeof(h->chksum));
	return sum == expected;
}

static isize tarfs_file_read(struct vfs_node *node, u64 offset, char *buffer,
                             usize size, int flags)
{
	(void)flags;
	if (!node || !node->inode || !node->inode->data)
		return -EINVAL;

	struct tarfs_file_info *info = (struct tarfs_file_info *)node->inode->data;
	if (offset >= info->size)
		return 0;

	if (offset + size > info->size)
		size = info->size - offset;

	if (size == 0)
		return 0;

	u64 byte_pos = info->start_lba * TAR_BLOCK_SIZE + offset;
	usize bytes_read = 0;
	char sector_buf[TAR_BLOCK_SIZE];

	while (bytes_read < size) {
		u64 cur_lba = (byte_pos + bytes_read) / TAR_BLOCK_SIZE;
		u32 cur_offset = (u32)((byte_pos + bytes_read) % TAR_BLOCK_SIZE);
		u32 chunk = TAR_BLOCK_SIZE - cur_offset;
		if (chunk > (size - bytes_read))
			chunk = (u32)(size - bytes_read);

		if (blk_read_cached(info->bdev, cur_lba, 1, sector_buf) < 0)
			return bytes_read > 0 ? (isize)bytes_read : -EIO;

		memcpy(buffer + bytes_read, sector_buf + cur_offset, chunk);
		bytes_read += chunk;
	}

	return (isize)bytes_read;
}

static struct vfs_node *tarfs_create_dir(struct vfs_node *parent, const char *name, u16 mode)
{
	struct vfs_node *dir = vfs_create_node(VFS_DIRECTORY);
	if (!dir)
		return NULL;

	strncpy(dir->name, name, sizeof(dir->name) - 1);
	dir->name[sizeof(dir->name) - 1] = '\0';
	dir->inode->mode = mode ? mode : 0755;
	dir->inode->nlink = 2;
	dir->parent = parent;
	dir->refcount++;
	vfs_attach_child(parent, dir);
	return dir;
}

static struct vfs_node *tarfs_create_file(struct vfs_node *parent, const char *name,
                                          struct block_device *bdev, u64 start_lba,
                                          u64 size, u16 mode)
{
	struct vfs_node *f = vfs_create_node(VFS_FILE);
	if (!f)
		return NULL;

	strncpy(f->name, name, sizeof(f->name) - 1);
	f->name[sizeof(f->name) - 1] = '\0';
	f->inode->mode = mode ? mode : 0644;
	f->inode->size = size;
	f->inode->nlink = 1;
	f->inode->flags |= VFS_NODE_OWNS_DATA;

	struct tarfs_file_info *info = kmalloc(sizeof(struct tarfs_file_info));
	if (!info) {
		vfs_node_put(f);
		return NULL;
	}
	info->bdev = bdev;
	info->start_lba = start_lba;
	info->size = size;

	f->inode->data = info;
	f->inode->read_cb = tarfs_file_read;
	f->parent = parent;
	f->refcount++;
	vfs_attach_child(parent, f);
	return f;
}

static void tarfs_add_entry(struct vfs_node *root, struct block_device *bdev,
                            const struct tar_header *h, u64 data_lba)
{
	char fullpath[256];
	fullpath[0] = '\0';

	if (h->prefix[0] != '\0') {
		strncpy(fullpath, h->prefix, sizeof(fullpath) - 1);
		fullpath[sizeof(fullpath) - 1] = '\0';
		usize len = strlen(fullpath);
		if (len < sizeof(fullpath) - 2 && fullpath[len - 1] != '/') {
			fullpath[len] = '/';
			fullpath[len + 1] = '\0';
		}
		strncat(fullpath, h->name, sizeof(fullpath) - strlen(fullpath) - 1);
	} else {
		strncpy(fullpath, h->name, sizeof(fullpath) - 1);
		fullpath[sizeof(fullpath) - 1] = '\0';
	}

	char *p = fullpath;
	while (*p == '.' && *(p + 1) == '/')
		p += 2;
	while (*p == '/')
		p++;

	if (*p == '\0')
		return;

	/* Strip trailing slash if any */
	usize plen = strlen(p);
	if (plen > 0 && p[plen - 1] == '/')
		p[plen - 1] = '\0';

	u64 size = parse_octal(h->size, sizeof(h->size));
	u16 mode = (u16)parse_octal(h->mode, sizeof(h->mode));
	char type = h->typeflag;

	struct vfs_node *curr = root;
	char *component = p;

	while (*component) {
		char *slash = strchr(component, '/');
		if (slash) {
			*slash = '\0';
			const char *dirname = component;
			component = slash + 1;

			if (dirname[0] == '\0')
				continue;

			struct vfs_node *child = find_child(curr, dirname);
			if (!child) {
				child = tarfs_create_dir(curr, dirname, 0755);
				if (!child)
					return;
			}
			curr = child;
		} else {
			/* Leaf entry */
			if (type == '5') {
				if (!find_child(curr, component))
					tarfs_create_dir(curr, component, mode);
			} else {
				if (!find_child(curr, component))
					tarfs_create_file(curr, component, bdev, data_lba, size, mode);
			}
			break;
		}
	}
}

static struct vfs_node *tarfs_mount_cb(const char *source, u64 flags, void *data)
{
	(void)flags;
	(void)data;

	struct block_device *bdev = blk_get(source);
	if (!bdev)
		return ERR_PTR(-ENODEV);

	struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
	if (!root)
		return ERR_PTR(-ENOMEM);

	root->inode->mode = 0755;
	root->inode->nlink = 2;

	u64 lba = 0;
	char buf[TAR_BLOCK_SIZE];
	int zero_blocks = 0;

	while (1) {
		if (blk_read_cached(bdev, lba, 1, buf) < 0)
			break;

		const struct tar_header *h = (const struct tar_header *)buf;
		if (h->name[0] == '\0') {
			zero_blocks++;
			if (zero_blocks >= 2)
				break;
			lba++;
			continue;
		}
		zero_blocks = 0;

		if (!tar_verify_checksum(h)) {
			/* Not a valid tar header or corrupted end */
			if (lba == 0) {
				vfs_node_put(root);
				return ERR_PTR(-EINVAL);
			}
			break;
		}

		u64 file_size = parse_octal(h->size, sizeof(h->size));
		u64 data_lba = lba + 1;

		tarfs_add_entry(root, bdev, h, data_lba);

		u64 data_sectors = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
		lba += 1 + data_sectors;
	}

	return root;
}

static struct vfs_fs tarfs_fs = {
	.name = "tarfs",
	.mount = tarfs_mount_cb,
	.flags = 0,
};

void tarfs_init(void)
{
	vfs_register_fs(&tarfs_fs);
	klog_info("TarFS: POSIX ustar archive filesystem registered\n");
}
