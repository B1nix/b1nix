/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fwcfgfs.c — QEMU fw_cfg Virtual Filesystem for b1nix.
 *
 * Exposes files and configuration blobs passed from the host into QEMU via
 * `-fw_cfg name=opt/foo,file=bar` as a structured virtual directory tree.
 * Enables zero-overhead host-to-guest file injection without needing initramfs
 * rebuilds, ISO repacking, or disk formatting.
 */

#include <b1nix/fwcfgfs.h>
#include <b1nix/fw_cfg.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

struct fwcfg_file_info {
	u16 select_key;
	u32 size;
};

struct fw_cfg_raw_entry {
	u32 size;
	u16 select;
	u16 reserved;
	char name[56];
};

static u32 be32(u32 v)
{
	return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) |
	       ((v >> 24) & 0xff);
}

static isize fwcfgfs_file_read(struct vfs_node *node, u64 offset, char *buffer,
                               usize size, int flags)
{
	(void)flags;
	if (!node || !node->inode || !node->inode->data)
		return -EINVAL;

	struct fwcfg_file_info *info = (struct fwcfg_file_info *)node->inode->data;
	if (offset >= info->size)
		return 0;

	if (offset + size > info->size)
		size = info->size - offset;

	if (size == 0)
		return 0;

	fw_cfg_select(info->select_key);

	/* Skip offset bytes if seeking */
	if (offset > 0) {
		char dummy[64];
		u64 to_skip = offset;
		while (to_skip > 0) {
			usize chunk = to_skip > sizeof(dummy) ? sizeof(dummy) : (usize)to_skip;
			fw_cfg_read(dummy, chunk);
			to_skip -= chunk;
		}
	}

	fw_cfg_read(buffer, size);
	return (isize)size;
}

static struct vfs_node *create_dir_node(struct vfs_node *parent, const char *name)
{
	struct vfs_node *dir = vfs_create_node(VFS_DIRECTORY);
	if (!dir)
		return NULL;

	strncpy(dir->name, name, sizeof(dir->name) - 1);
	dir->name[sizeof(dir->name) - 1] = '\0';
	dir->inode->mode = 0755;
	dir->inode->nlink = 2;
	dir->parent = parent;
	dir->refcount++;
	vfs_attach_child(parent, dir);
	return dir;
}

static struct vfs_node *create_file_node(struct vfs_node *parent, const char *name,
                                         u16 select_key, u32 size)
{
	struct vfs_node *f = vfs_create_node(VFS_DEVICE);
	if (!f)
		return NULL;

	strncpy(f->name, name, sizeof(f->name) - 1);
	f->name[sizeof(f->name) - 1] = '\0';
	f->inode->mode = 0444;
	f->inode->size = size;
	f->inode->nlink = 1;
	f->inode->flags |= VFS_NODE_PSEUDO_REG | VFS_NODE_OWNS_DATA;

	struct fwcfg_file_info *info = kmalloc(sizeof(struct fwcfg_file_info));
	if (!info) {
		vfs_node_put(f);
		return NULL;
	}
	info->select_key = select_key;
	info->size = size;

	f->inode->data = info;
	f->inode->read_cb = fwcfgfs_file_read;
	f->parent = parent;
	f->refcount++;
	vfs_attach_child(parent, f);
	return f;
}

static void fwcfgfs_add_path(struct vfs_node *root, const char *path,
                             u16 select_key, u32 size)
{
	char buf[64];
	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	struct vfs_node *curr = root;
	char *p = buf;

	while (*p) {
		char *slash = strchr(p, '/');
		if (slash) {
			*slash = '\0';
			const char *dirname = p;
			p = slash + 1;

			if (dirname[0] == '\0')
				continue;

			struct vfs_node *child = find_child(curr, dirname);
			if (!child) {
				child = create_dir_node(curr, dirname);
				if (!child)
					return;
			}
			curr = child;
		} else {
			/* Final component: file name */
			if (p[0] != '\0') {
				if (!find_child(curr, p)) {
					create_file_node(curr, p, select_key, size);
				}
			}
			break;
		}
	}
}

static struct vfs_node *fwcfgfs_mount_cb(const char *source, u64 flags, void *data)
{
	(void)source;
	(void)flags;
	(void)data;

	if (!fw_cfg_present())
		return ERR_PTR(-ENODEV);

	struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
	if (!root)
		return ERR_PTR(-ENOMEM);

	root->inode->mode = 0755;
	root->inode->nlink = 2;

	u32 count;
	fw_cfg_select(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);

	if (count > 512)
		count = 512;

	for (u32 i = 0; i < count; i++) {
		struct fw_cfg_raw_entry entry;
		fw_cfg_read(&entry, sizeof(entry));
		entry.name[sizeof(entry.name) - 1] = '\0';

		u16 key = (u16)((entry.select >> 8) | (entry.select << 8));
		u32 sz = be32(entry.size);

		fwcfgfs_add_path(root, entry.name, key, sz);
	}

	return root;
}

static struct vfs_fs fwcfgfs_fs = {
	.name = "fwcfgfs",
	.mount = fwcfgfs_mount_cb,
	.flags = VFS_FS_NODEV,
};

void fwcfgfs_init(void)
{
	vfs_register_fs(&fwcfgfs_fs);
	klog_info("FwCfgFS: QEMU firmware configuration filesystem registered\n");
}
