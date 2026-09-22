/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The bridge, b1nix side: a VFS filesystem served by imported Linux code.
 *
 * `mount -t btrfs /dev/sda /mnt` mounts a real btrfs with the unmodified
 * Linux filesystem compiled into this kernel (see docs/filesystems-and-storage.md), and
 * every path under it is served by that code — open, read, write, readdir,
 * create, unlink, rename and the rest.
 *
 * The type is deliberately NOT called "btrfs": b1nix's own driver keeps that
 * name, and the two are useful side by side while the import is young. A mount
 * of either is a real mount of the same on-disk format.
 *
 * Nothing here includes a Linux header. Every operation goes through
 * kernel/lkpi/fs_bridge.c, which is compiled on the other side of the boundary
 * <lkpi/env.h> describes, and a handle is an opaque pointer this file never
 * looks inside.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/mm.h>
#include <b1nix/spinlock.h>
#include <b1nix/page_cache.h>
#include <b1nix/posix.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

#include "../lkpi/fs_bridge.h"

/* Per-file state, hanging off inode->data. */
struct lkpifs_node {
	void *handle;             /* the imported filesystem's dentry */
	const char *linux_name;   /* which type, for diagnostics */
};

static void lkpifs_release(struct vfs_node *node);

/*
 * Only a node whose ops this file installed carries a lkpifs_node. The VFS
 * gives a node it creates its own data first -- a new symlink holds its
 * target string there -- and reading that as a handle dereferenced the
 * string's bytes.
 */
static struct lkpifs_node *node_info(struct vfs_node *node)
{
	if (!node || !node->inode || node->inode->release_cb != lkpifs_release)
		return 0;
	return (struct lkpifs_node *)node->inode->data;
}

/*
 * The imported filesystem's handle for a node, materialised on demand.
 *
 * A node can be left without one: lkpifs_drop_child() releases the handle for
 * a name that has just stopped existing — the source of a rename, or anything
 * still cached at unmount — so that the filesystem sees the last reference go
 * and can evict the inode. The node itself stays in b1nix's tree, and a name
 * that comes back (a rename's destination is the same node under a new name)
 * has to be usable again: without this, the first operation after a rename
 * found a NULL handle and returned -EINVAL, which is how a truncate of a
 * just-renamed file failed.
 *
 * Looking it up again from the parent is the same lazy materialisation
 * lookup_cb does, and the parent is materialised the same way, up to the root
 * whose handle belongs to the mount.
 */
static void *node_handle(struct vfs_node *node)
{
	struct lkpifs_node *info = node_info(node);

	if (!info)
		return 0;
	if (!info->handle && node->parent && node->name[0]) {
		void *parent = node_handle(node->parent);

		if (parent)
			info->handle = lkpi_bridge_lookup(parent, node->name);
	}
	return info->handle;
}

/* ── building a VFS node over a handle ──────────────────────────── */

static enum vfs_node_type type_of_mode(unsigned int mode)
{
	switch (mode & 0170000u) {
	case 0040000u:
		return VFS_DIRECTORY;
	case 0120000u:
		return VFS_SYMLINK;
	case 0010000u:
		return VFS_FIFO;
	case 0140000u:
		return VFS_SOCKET;
	case 0020000u:
	case 0060000u:
		return VFS_DEVICE;
	default:
		return VFS_FILE;
	}
}

static isize lkpifs_read(struct vfs_node *node, u64 offset, char *buffer,
                         usize size, int flags);
static isize lkpifs_write(struct vfs_node *node, u64 offset,
                          const char *buffer, usize size, int flags);
static int lkpifs_write_through(struct vfs_node *node);
static isize lkpifs_readdir_at(struct vfs_node *dir, u64 cookie,
                               struct dirent *buf, usize max_entries,
                               u64 *next_cookie);
static int lkpifs_lookup(struct vfs_node *dir, const char *name);
static int lkpifs_create(struct vfs_node *dir, const char *name,
                         const char *full_path, u32 mode);
static int lkpifs_mkdir(struct vfs_node *dir, const char *name, u32 mode);
static int lkpifs_mknod(struct vfs_node *dir, const char *name, u32 mode);
static int lkpifs_unlink(struct vfs_node *dir, const char *name);
static int lkpifs_rmdir(struct vfs_node *dir, const char *name);
static int lkpifs_rename(struct vfs_node *old_dir, const char *old_name,
                         struct vfs_node *new_dir, const char *new_name);
static int lkpifs_link(struct vfs_node *target, struct vfs_node *dir,
                       const char *name);
static int lkpifs_symlink(struct vfs_node *dir, const char *name,
                          const char *target);
static int lkpifs_truncate(struct vfs_node *node, u64 length);
static int lkpifs_setattr(struct vfs_node *node);
static int lkpifs_statfs(struct vfs_node *node, struct b1nix_statfs *st);
static int lkpifs_fitrim(struct vfs_node *node, u64 start, u64 len, u64 minlen,
                         u64 *trimmed);
static int lkpifs_fsync(struct vfs_node *node);
/* ── extended attributes ────────────────────────────────────────── */

/*
 * Straight through to the filesystem. b1nix keeps a per-inode list of its own
 * for filesystems that store nothing themselves, and using it here would lose
 * every attribute at unmount and hide the ones already on the disk.
 */
/* Is `name` one of the NUL-separated names a listxattr call returned? */
static int lkpifs_list_has(const char *list, usize len, const char *name)
{
	usize off = 0;

	while (off < len) {
		usize n = strlen(list + off);

		if (strcmp(list + off, name) == 0)
			return 1;
		off += n + 1;
	}
	return 0;
}

static isize lkpifs_getxattr(struct vfs_node *node, const char *name,
                             void *value, usize size)
{
	void *handle = node_handle(node);

	if (!handle || !name)
		return -EINVAL;
	return (isize)lkpi_bridge_getxattr(handle, name, value,
	                                   (unsigned long)size);
}

static int lkpifs_setxattr(struct vfs_node *node, const char *name,
                           const void *value, usize size, int flags)
{
	void *handle = node_handle(node);

	if (!handle || !name)
		return -EINVAL;
	return lkpi_bridge_setxattr(handle, name, value, (unsigned long)size,
	                            flags);
}

static int lkpifs_removexattr(struct vfs_node *node, const char *name)
{
	void *handle = node_handle(node);

	if (!handle || !name)
		return -EINVAL;
	return lkpi_bridge_removexattr(handle, name);
}

static isize lkpifs_listxattr(struct vfs_node *node, char *list, usize size)
{
	void *handle = node_handle(node);

	if (!handle)
		return -EINVAL;
	return (isize)lkpi_bridge_listxattr(handle, list, (unsigned long)size);
}

static void lkpifs_release(struct vfs_node *node);
static void lkpifs_getattr(struct vfs_node *node);
static isize lkpifs_getxattr(struct vfs_node *node, const char *name,
                             void *value, usize size);
static int lkpifs_setxattr(struct vfs_node *node, const char *name,
                           const void *value, usize size, int flags);
static int lkpifs_removexattr(struct vfs_node *node, const char *name);
static isize lkpifs_listxattr(struct vfs_node *node, char *list, usize size);

/* Copy what the imported filesystem says about a file onto the VFS inode. */
static void apply_attr(struct vfs_node *node, const struct lkpi_bridge_attr *a)
{
	node->inode->ino = a->ino;
	node->inode->size = (usize)a->size;
	node->inode->mode = (u16)(a->mode & 07777u);
	node->inode->nlink = (int)a->nlink;
	node->inode->uid = (u32)a->uid;
	node->inode->gid = (u32)a->gid;
	node->inode->atime = a->atime;
	node->inode->mtime = a->mtime;
	node->inode->ctime = a->ctime;
	node->inode->atime_nsec = a->atime_nsec;
	node->inode->mtime_nsec = a->mtime_nsec;
	node->inode->ctime_nsec = a->ctime_nsec;
	/* The attribute byte the VFS enforces (immutable, append-only) is the
	 * filesystem's; keep the rest of attr, which the VFS owns. */
	node->inode->attr = (node->inode->attr & ~VFS_ATTR_USER_MASK) |
	                    (a->flags & VFS_ATTR_USER_MASK);
}

static int lkpifs_setflags(struct vfs_node *node, u32 attr)
{
	void *handle = node_handle(node);

	if (!handle)
		return -EINVAL;
	return lkpi_bridge_set_flags(handle, attr & VFS_ATTR_USER_MASK);
}

static void install_ops(struct vfs_node *node, void *handle,
                        const char *linux_name)
{
	struct lkpifs_node *info = kzalloc(sizeof(*info));

	if (info) {
		info->handle = handle;
		info->linux_name = linux_name;
		node->inode->data = info;
		/* Not VFS_NODE_OWNS_DATA: the handle has to be released to the
		 * imported filesystem before the memory goes, which is what
		 * release_cb below does. */
	}

	node->inode->read_cb = lkpifs_read;
	node->inode->write_cb = lkpifs_write;
	node->inode->write_through_cb = lkpifs_write_through;
	node->inode->readdir_at_cb = lkpifs_readdir_at;
	node->inode->lookup_cb = lkpifs_lookup;
	node->inode->create_cb = lkpifs_create;
	node->inode->mkdir_cb = lkpifs_mkdir;
	node->inode->mknod_cb = lkpifs_mknod;
	node->inode->unlink_cb = lkpifs_unlink;
	node->inode->rmdir_cb = lkpifs_rmdir;
	node->inode->rename_cb = lkpifs_rename;
	node->inode->link_cb = lkpifs_link;
	node->inode->symlink_cb = lkpifs_symlink;
	node->inode->truncate_cb = lkpifs_truncate;
	node->inode->setattr_cb = lkpifs_setattr;
	node->inode->fsync_cb = lkpifs_fsync;
	node->inode->release_cb = lkpifs_release;
	node->inode->getattr_cb = lkpifs_getattr;
	node->inode->setflags_cb = lkpifs_setflags;
	node->inode->statfs_cb = lkpifs_statfs;
	node->inode->fitrim_cb = lkpifs_fitrim;
	node->inode->getxattr_cb = lkpifs_getxattr;
	node->inode->setxattr_cb = lkpifs_setxattr;
	node->inode->removexattr_cb = lkpifs_removexattr;
	node->inode->listxattr_cb = lkpifs_listxattr;
	/*
	 * The directory listing comes from the filesystem, and the nodes this
	 * file materialises are a cache of the same names. Without this the VFS
	 * merges the two and every looked-up name is listed twice.
	 */
	node->inode->readdir_lists_children = 1;
}

/*
 * Build the VFS node for a handle the imported filesystem has just given us.
 * The handle is consumed either way: on failure it is released here.
 */
static struct vfs_node *make_node(void *handle, const char *name,
                                  const char *linux_name)
{
	struct lkpi_bridge_attr a;
	struct vfs_node *node;

	if (lkpi_bridge_attr(handle, &a) != 0) {
		lkpi_bridge_put(handle);
		return 0;
	}
	node = vfs_create_node(type_of_mode(a.mode));
	if (!node) {
		lkpi_bridge_put(handle);
		return 0;
	}
	if (name) {
		strncpy(node->name, name, VFS_NAME_MAX - 1);
		node->name[VFS_NAME_MAX - 1] = '\0';
	}
	apply_attr(node, &a);
	install_ops(node, handle, linux_name);
	return node;
}

/* ── mounting ───────────────────────────────────────────────────── */

/* Mounted roots, for sync(2). The VFS's own sync only reaches filesystems it
 * knows by name and the block cache; an imported filesystem keeps its changes
 * in a transaction until its sync_fs commits it, so without this a sync left
 * everything since the last periodic commit off the disk. */
#define LKPIFS_MAX_MOUNTS 32
static void *g_lkpifs_roots[LKPIFS_MAX_MOUNTS];
static spinlock_t g_lkpifs_roots_lock = SPINLOCK_INIT;

static void lkpifs_roots_set(void *old, void *new)
{
	u64 flags;

	spin_lock_irqsave(&g_lkpifs_roots_lock, &flags);
	for (int i = 0; i < LKPIFS_MAX_MOUNTS; i++)
		if (g_lkpifs_roots[i] == old) {
			g_lkpifs_roots[i] = new;
			break;
		}
	spin_unlock_irqrestore(&g_lkpifs_roots_lock, flags);
}

void lkpifs_sync_all(void)
{
	for (int i = 0; i < LKPIFS_MAX_MOUNTS; i++) {
		u64 flags;
		void *root;

		spin_lock_irqsave(&g_lkpifs_roots_lock, &flags);
		root = g_lkpifs_roots[i];
		spin_unlock_irqrestore(&g_lkpifs_roots_lock, flags);
		/* sync_fs sleeps; an unmount racing a sync is the caller's to
		 * exclude, as on Linux (the mount is busy while synced). */
		if (root)
			lkpi_bridge_sync_fs(root);
	}
}

static int lkpifs_remount(struct vfs_node *root, u64 flags)
{
	return lkpi_bridge_remount(node_handle(root), (flags & MS_RDONLY) ? 1ul : 0ul);
}

static struct vfs_node *lkpifs_mount_type(const char *linux_name,
                                          const char *source, u64 flags)
{
	void *root_handle;
	struct vfs_node *root;
	char msg[128];

	if (!source)
		return ERR_PTR(-EINVAL);
	/*
	 * SB_RDONLY is 1 in Linux's flags, which is also b1nix's read-only mount
	 * bit; anything else b1nix passes is not a Linux mount flag and is
	 * deliberately not forwarded.
	 */
	root_handle = lkpi_bridge_mount(linux_name, source,
	                                (flags & MS_RDONLY) ? 1ul : 0ul);
	if (!root_handle) {
		extern int lkpi_bridge_last_mount_error;
		extern int lkpi_mount_stage;

		snprintf(msg, sizeof(msg),
		         "lkpifs: %s could not mount %s through the imported code "
		         "(err %d at stage %d)",
		         linux_name, source, lkpi_bridge_last_mount_error,
		         lkpi_mount_stage);
		klog_warn(msg);
		return ERR_PTR(-EINVAL);
	}

	root = make_node(root_handle, "/", linux_name);
	if (!root) {
		lkpi_bridge_unmount(root_handle);
		return ERR_PTR(-ENOMEM);
	}
	root->inode->remount_cb = lkpifs_remount;
	lkpifs_roots_set(0, root_handle);
	/* The root's handle belongs to the mount, not to the node: releasing it
	 * is an unmount, which umount_cb does. */
	snprintf(msg, sizeof(msg), "lkpifs: mounted %s from %s (imported %s)",
	         linux_name, source, linux_name);
	klog_info(msg);
	return root;
}

/*
 * Release the imported filesystem's handles held by this subtree.
 *
 * Every node materialised under the mount keeps a dentry of the imported
 * filesystem alive, and those nodes outlive the unmount: b1nix frees them with
 * the rest of its tree afterwards. The imported side must not be unmounted
 * while its dentries are still referenced — an inode whose last reference has
 * not gone never reaches evict_inode, so a file unlinked during the session
 * stayed on ext4's orphan list with its bitmap bit set and no deletion time,
 * which the host's e2fsck reports as a deleted inode with zero dtime.
 */
static void lkpifs_release_subtree(struct vfs_node *node)
{
	struct vfs_node *child;

	if (!node)
		return;
	for (child = node->first_child; child; child = child->next_sibling)
		lkpifs_release_subtree(child);
	{
		struct lkpifs_node *info = node_info(node);

		if (info && info->handle) {
			lkpi_bridge_put(info->handle);
			info->handle = 0;
		}
	}
}

/* Write out what the page cache still holds for this subtree. vfs_umount's own
 * writeback runs after the umount callback, by which time the handles its
 * writes would go through are gone: a file written and unmounted without a
 * sync came back empty. */
static void lkpifs_flush_subtree(struct vfs_node *node)
{
	struct vfs_node *child;

	if (!node)
		return;
	for (child = node->first_child; child; child = child->next_sibling)
		lkpifs_flush_subtree(child);
	if (node->inode && node_info(node))
		page_cache_flush_inode(node->inode);
}

static int lkpifs_umount(struct vfs_node *root)
{
	struct lkpifs_node *info = node_info(root);
	void *handle;
	struct vfs_node *child;

	if (!info)
		return 0;
	lkpifs_flush_subtree(root);
	for (child = root->first_child; child; child = child->next_sibling)
		lkpifs_release_subtree(child);
	handle = info->handle;
	info->handle = 0;
	if (handle) {
		lkpifs_roots_set(handle, 0);
		lkpi_bridge_unmount(handle);
	}
	return 0;
}

/* ── the operations ─────────────────────────────────────────────── */

static isize lkpifs_read(struct vfs_node *node, u64 offset, char *buffer,
                         usize size, int flags)
{
	void *handle = node_handle(node);
	long ret;

	(void)flags;
	if (!handle || !buffer)
		return -EINVAL;
	/*
	 * A symlink's "contents" are its target: the VFS renders one by calling
	 * read_cb, which is how a filesystem with no in-memory target string
	 * answers readlink.
	 */
	if (node->inode->type == VFS_SYMLINK) {
		/* Honour the offset: a reader looping until 0 got the target
		 * again at every offset. */
		char *target = kmalloc(VFS_MAX_PATH);
		int n;

		if (!target)
			return -ENOMEM;
		n = lkpi_bridge_readlink(handle, target, VFS_MAX_PATH);
		if (n < 0 || offset >= (u64)n) {
			kfree(target);
			return n < 0 ? n : 0;
		}
		if (size > (usize)n - (usize)offset)
			size = (usize)n - (usize)offset;
		memcpy(buffer, target + offset, size);
		kfree(target);
		return (isize)size;
	}
	ret = lkpi_bridge_read(handle, offset, buffer, size);
	return (isize)ret;
}

static isize lkpifs_write(struct vfs_node *node, u64 offset,
                          const char *buffer, usize size, int flags)
{
	void *handle = node_handle(node);
	long ret;

	(void)flags;
	if (!handle || !buffer)
		return -EINVAL;
	ret = lkpi_bridge_write(handle, offset, buffer, size);
	if (ret > 0) {
		u64 end = offset + (u64)ret;

		if (end > node->inode->size)
			node->inode->size = (usize)end;
	}
	return (isize)ret;
}

/* A filesystem enforcing quota limits refuses a write that would exceed one,
 * and that refusal is write(2)'s EDQUOT only if the write reaches it. */
static int lkpifs_write_through(struct vfs_node *node)
{
	void *handle = node_handle(node);

	return handle && lkpi_bridge_quota_enforced(handle);
}

struct lkpifs_dir_fill {
	struct dirent *buf;
	usize max;
	usize count;
};

static int lkpifs_emit(void *arg, const char *name, int len,
                       unsigned long long ino, unsigned int type)
{
	struct lkpifs_dir_fill *f = arg;
	usize n = (usize)len;

	if (f->count >= f->max)
		return 0;   /* stop: no room, and the cursor stays where it is */
	/* "." and ".." are the VFS's own business; a filesystem that reported
	 * them would have them listed twice. */
	if ((n == 1 && name[0] == '.') ||
	    (n == 2 && name[0] == '.' && name[1] == '.'))
		return 1;
	if (n > sizeof(f->buf[0].name) - 1)
		n = sizeof(f->buf[0].name) - 1;
	memcpy(f->buf[f->count].name, name, n);
	f->buf[f->count].name[n] = '\0';
	/* DT_DIR is 4 and DT_LNK 10 in the getdents ABI the filesystem reports
	 * against; everything else is listed as a plain file, which is what the
	 * caller's stat then corrects. */
	f->buf[f->count].type = (u32)(type == 4 ? VFS_DIRECTORY
	                              : type == 10 ? VFS_SYMLINK
	                              : type == 1 ? VFS_FIFO
	                                          : VFS_FILE);
	f->buf[f->count].is_dir = (type == 4);
	f->buf[f->count].is_exec = 0;
	f->buf[f->count].size = 0;
	f->buf[f->count].ino = ino;
	f->count++;
	return 1;
}

static isize lkpifs_readdir_at(struct vfs_node *dir, u64 cookie,
                               struct dirent *buf, usize max_entries,
                               u64 *next_cookie)
{
	void *handle = node_handle(dir);
	struct lkpifs_dir_fill fill = { buf, max_entries, 0 };
	unsigned long long next = cookie;
	int ret;

	if (!handle || !buf)
		return -EINVAL;
	if (next_cookie)
		*next_cookie = cookie;
	ret = lkpi_bridge_iterate(handle, cookie, &next, lkpifs_emit, &fill);
	if (ret < 0)
		return ret;
	if (next_cookie)
		*next_cookie = next;
	return (isize)fill.count;
}

static int lkpifs_lookup(struct vfs_node *dir, const char *name)
{
	struct lkpifs_node *info = node_info(dir);
	void *handle;
	struct vfs_node *child;

	if (!info || !info->handle || !name)
		return -EINVAL;
	handle = lkpi_bridge_lookup(info->handle, name);
	if (!handle)
		return -ENOENT;
	child = make_node(handle, name, info->linux_name);
	if (!child)
		return -ENOMEM;
	/* The page cache keys pages by (fs_id, ino). A node made on lookup is
	 * allocated after the mount, with fs_id 0 -- which it then shared with
	 * every in-memory filesystem, and a btrfs file whose inode number matched
	 * a tmpfs one read that file's cached pages. */
	child->inode->fs_id = dir->inode->fs_id;
	if (!vfs_attach_child_unique(dir, child)) {
		/* Someone else's node for the name got there first. */
		child->deleted = 1;
		__atomic_store_n(&child->refcount, 1, __ATOMIC_RELAXED);
		vfs_node_put(child);
	}
	return 0;
}

static int lkpifs_create(struct vfs_node *dir, const char *name,
                         const char *full_path, u32 mode)
{
	struct lkpifs_node *info = node_info(dir);
	struct vfs_node *child;
	void *handle;
	int rc;

	(void)full_path;
	if (!info || !info->handle || !name)
		return -EINVAL;
	rc = lkpi_bridge_create(info->handle, name, (mode & 07777u) | 0100000u);
	if (rc)
		return rc;
	/*
	 * The VFS has already linked a child node for the name; give it the
	 * handle the filesystem just created, so the first write does not have
	 * to look it up again.
	 */
	child = find_child(dir, name);
	if (child && child->inode && !child->inode->data) {
		handle = lkpi_bridge_lookup(info->handle, name);
		if (handle) {
			struct lkpi_bridge_attr a;

			if (lkpi_bridge_attr(handle, &a) == 0)
				apply_attr(child, &a);
			install_ops(child, handle, info->linux_name);
		}
	}
	if (child) {
		child->inode->fs_id = dir->inode->fs_id; /* see lkpifs_lookup */
		page_cache_invalidate_stale(child->inode);
		vfs_node_put(child);
	}
	return 0;
}

static int lkpifs_mkdir(struct vfs_node *dir, const char *name, u32 mode)
{
	struct lkpifs_node *info = node_info(dir);
	struct vfs_node *child;
	int rc;

	if (!info || !info->handle || !name)
		return -EINVAL;
	rc = lkpi_bridge_mkdir(info->handle, name, (mode & 07777u) | 0040000u);
	if (rc)
		return rc;
	child = find_child(dir, name);
	if (child && child->inode && !node_info(child)) {
		void *handle = lkpi_bridge_lookup(info->handle, name);

		if (handle) {
			install_ops(child, handle, info->linux_name);
		}
	}
	if (child) {
		child->inode->fs_id = dir->inode->fs_id; /* see lkpifs_lookup */
		vfs_node_put(child);
	}
	return 0;
}

/* A FIFO as a real inode. vfs_mknod has already put the node in place and
 * keeps its type VFS_FIFO, so opens go to the pipe path; the ops attached
 * here serve its attributes and its unlink. */
static int lkpifs_mknod(struct vfs_node *dir, const char *name, u32 mode)
{
	struct lkpifs_node *info = node_info(dir);
	struct vfs_node *child;
	int rc;

	if (!info || !info->handle || !name)
		return -EINVAL;
	rc = lkpi_bridge_mknod(info->handle, name, (mode & 07777u) | 0010000u);
	if (rc)
		return rc;
	child = find_child(dir, name);
	if (child && child->inode && !node_info(child)) {
		void *handle = lkpi_bridge_lookup(info->handle, name);

		if (handle)
			install_ops(child, handle, info->linux_name);
	}
	if (child) {
		child->inode->fs_id = dir->inode->fs_id; /* see lkpifs_lookup */
		vfs_node_put(child);
	}
	return 0;
}

/*
 * Release the imported filesystem's handle for a name that is about to stop
 * existing.
 *
 * The VFS calls release_cb only when a node is freed with its "deleted" flag
 * set, and a node that merely stops being reachable — the source name of a
 * rename, or anything still cached when the mount is torn down — never gets
 * there. The dentry it holds keeps the imported inode referenced, so the last
 * iput never comes and the filesystem never evicts it: an ext4 file unlinked
 * during the session stayed allocated on disk, with its bitmap bit set, no
 * deletion time and a stale entry in the orphan file. Dropping the handle here
 * is what makes the eviction happen while the filesystem is still mounted.
 *
 * The node stays; it materialises its handle again through lookup_cb if the
 * name comes back.
 */
/*
 * `keep_if_held` is for unlink and nothing else.
 *
 * An unlinked file is still a file: the descriptor holding it open can be
 * truncated, written and read, and only the name is gone. Dropping the handle
 * there left the node with nothing to reach the filesystem through, so it tried
 * to look the name up again -- and the name is exactly what unlink removed, so
 * every operation on that descriptor answered EINVAL (liburing's rw_merge_test
 * opens a scratch file, unlinks it and ftruncates it, which is an ordinary
 * shape). So the handle survives while anything still holds the node, and
 * release_cb drops it when the last holder goes, which is also when the
 * filesystem should evict the inode.
 *
 * Rename must NOT do that. The name is gone there too, but the node behind the
 * OLD name has to re-materialise under the new one, and a node that kept its
 * old dentry read the wrong inode: `cp a b; mv b c` and the churn test's
 * rename-over-a-previous-version both came back with the wrong contents.
 */
static void lkpifs_drop_child(struct vfs_node *dir, const char *name,
                              int keep_if_held)
{
	struct vfs_node *child;
	struct lkpifs_node *info;

	if (!dir || !name)
		return;
	child = find_child(dir, name);
	if (!child)
		return;
	/* Still open? Then the handle stays. The node's refcount cannot answer
	 * that -- the page cache and the name cache hold references of their own,
	 * and treating those as "open" delayed the eviction the churn test waits
	 * for. The count of open file descriptions can. */
	if (keep_if_held && vfs_inode_is_open(child->inode)) {
		vfs_node_put(child);
		return;
	}
	info = node_info(child);
	if (info && info->handle) {
		lkpi_bridge_put(info->handle);
		info->handle = 0;
	}
	vfs_node_put(child);
}

static int lkpifs_unlink(struct vfs_node *dir, const char *name)
{
	struct lkpifs_node *info = node_info(dir);

	int ret;

	if (!info || !info->handle || !name)
		return -EINVAL;
	ret = lkpi_bridge_unlink(info->handle, name);
	if (ret == 0)
		lkpifs_drop_child(dir, name, 1);
	return ret;
}

static int lkpifs_rmdir(struct vfs_node *dir, const char *name)
{
	struct lkpifs_node *info = node_info(dir);

	int ret;

	if (!info || !info->handle || !name)
		return -EINVAL;
	ret = lkpi_bridge_rmdir(info->handle, name);
	if (ret == 0)
		lkpifs_drop_child(dir, name, 0);
	return ret;
}

static int lkpifs_rename(struct vfs_node *old_dir, const char *old_name,
                         struct vfs_node *new_dir, const char *new_name)
{
	struct lkpifs_node *from = node_info(old_dir);
	struct lkpifs_node *to = node_info(new_dir);

	int ret;

	if (!from || !to || !from->handle || !to->handle)
		return -EINVAL;
	/* The source's handle is dropped below, and writeback finds a file's
	 * handle only through it: a page still dirty after the rename had no
	 * way back to the filesystem, and `cp a b; mv b c` left c empty. */
	{
		struct vfs_node *src = find_child(old_dir, old_name);

		if (src) {
			page_cache_flush_inode(src->inode);
			vfs_node_put(src);
		}
	}
	ret = lkpi_bridge_rename(from->handle, old_name, to->handle, new_name);
	if (ret == 0) {
		/* Both names change hands: the source stops existing, and anything
		 * that was at the destination has been replaced. */
		lkpifs_drop_child(old_dir, old_name, 0);
		lkpifs_drop_child(new_dir, new_name, 0);
	}
	return ret;
}

static int lkpifs_link(struct vfs_node *target, struct vfs_node *dir,
                       const char *name)
{
	struct lkpifs_node *info = node_info(dir);
	void *target_handle = node_handle(target);

	if (!info || !info->handle || !target_handle || !name)
		return -EINVAL;
	return lkpi_bridge_link(info->handle, name, target_handle);
}

static int lkpifs_symlink(struct vfs_node *dir, const char *name,
                          const char *target)
{
	struct lkpifs_node *info = node_info(dir);
	struct vfs_node *child;
	int rc;

	if (!info || !info->handle || !name || !target)
		return -EINVAL;
	rc = lkpi_bridge_symlink(info->handle, name, target);
	if (rc)
		return rc;
	child = find_child(dir, name);
	if (child && child->inode && !node_info(child)) {
		void *handle = lkpi_bridge_lookup(info->handle, name);

		if (handle) {
			/* The VFS stored the target itself; the filesystem owns it now
			 * and read_cb answers readlink from there. */
			if (child->inode->flags & VFS_NODE_OWNS_DATA) {
				kfree(child->inode->data);
				child->inode->flags &= ~VFS_NODE_OWNS_DATA;
			}
			child->inode->data = 0;
			install_ops(child, handle, info->linux_name);
		}
	}
	if (child) {
		child->inode->fs_id = dir->inode->fs_id; /* see lkpifs_lookup */
		vfs_node_put(child);
	}
	return 0;
}

static int lkpifs_truncate(struct vfs_node *node, u64 length)
{
	void *handle = node_handle(node);
	int rc;

	if (!handle)
		return -EINVAL;
	rc = lkpi_bridge_truncate(handle, length);
	if (rc == 0)
		node->inode->size = (usize)length;
	return rc;
}

static int lkpifs_setattr(struct vfs_node *node)
{
	void *handle = node_handle(node);

	if (!handle)
		return -EINVAL;
	/* b1nix's setattr_cb says "the inode has changed": push what the
	 * filesystem stores -- mode, owner, timestamps. Pending writes go first,
	 * or reaching the filesystem afterwards they would stamp a new mtime over
	 * the one set here (utime after write). */
	page_cache_flush_inode(node->inode);
	return lkpi_bridge_setattr(handle, node->inode->mode, node->inode->uid,
	                           node->inode->gid, node->inode->atime,
	                           node->inode->mtime);
}

static int lkpifs_fitrim(struct vfs_node *node, u64 start, u64 len, u64 minlen,
                         u64 *trimmed)
{
	void *handle = node_handle(node);
	unsigned long long t = 0;
	int rc;

	if (!handle)
		return -EINVAL;
	rc = lkpi_bridge_fitrim(handle, start, len, minlen, &t);
	if (rc == 0)
		*trimmed = t;
	return rc;
}

static int lkpifs_statfs(struct vfs_node *node, struct b1nix_statfs *st)
{
	struct lkpi_bridge_statfs b;
	void *handle = node_handle(node);
	int rc;

	if (!handle || !st)
		return -EINVAL;
	rc = lkpi_bridge_statfs(handle, &b);
	if (rc)
		return rc;
	st->f_type = b.type;
	st->f_bsize = b.bsize;
	st->f_blocks = b.blocks;
	st->f_bfree = b.bfree;
	st->f_bavail = b.bavail;
	st->f_files = b.files;
	st->f_ffree = b.ffree;
	st->f_fsid = b.fsid;
	st->f_namelen = b.namelen;
	st->f_frsize = b.frsize;
	st->f_flags = b.flags;
	return 0;
}

static int lkpifs_fsync(struct vfs_node *node)
{
	void *handle = node_handle(node);

	if (!handle) {
		/*
		 * The name is gone. lkpifs_drop_child released the imported
		 * filesystem's handle when the file was unlinked, deliberately,
		 * so the inode could be evicted while the mount is still live
		 * (the note above it says why); a descriptor already open on
		 * that file keeps working, and fsync(2) on it must keep working
		 * too. It returns 0 on Linux, and EINVAL here is what liburing's
		 * sync_file_range test found.
		 *
		 * There is nothing of the file's own left to sync: vfs_fsync_h
		 * flushed its page cache and wrote this inode's blocks back
		 * before calling in here. What is left is the barrier, and the
		 * superblock can still issue that — reached through the nearest
		 * ancestor that does have a handle.
		 */
		struct vfs_node *p = node->parent;

		while (p) {
			void *ph = node_handle(p);

			if (ph)
				return lkpi_bridge_sync_fs(ph);
			p = p->parent;
		}
		return 0;
	}
	if (node->inode->type == VFS_DIRECTORY)
		return lkpi_bridge_sync_fs(handle);
	return lkpi_bridge_sync(handle);
}

static void lkpifs_getattr(struct vfs_node *node)
{
	struct lkpi_bridge_attr a;
	void *handle = node_handle(node);

	/* The imported filesystem owns these: a size or a link count that changed
	 * behind the VFS's back (another name for the same inode, a write through
	 * a second path) is read here rather than remembered from the lookup. */
	if (!handle)
		return;
	/*
	 * Writes land in the VFS page cache first and reach the filesystem at
	 * writeback. Until then its size is the old one, and copying that over the
	 * VFS inode made writeback and every read stop at it: a file written and
	 * then stat'ed came out empty. Hand the dirty pages over before asking.
	 */
	page_cache_flush_inode(node->inode);
	if (lkpi_bridge_attr(handle, &a) == 0)
		apply_attr(node, &a);
}

static void lkpifs_release(struct vfs_node *node)
{
	struct lkpifs_node *info = node_info(node);

	if (!info)
		return;
	node->inode->data = 0;
	if (info->handle)
		lkpi_bridge_put(info->handle);
	kfree(info);
}

/* ── quotas ─────────────────────────────────────────────────────── */

/* quotactl(2) on the filesystem `on_fs` lives on; see kernel/lkpi/fs_quotactl.c.
 * A node on any other filesystem has no quota operations, which Linux reports
 * as ENOSYS. */
int lkpifs_quotactl(struct vfs_node *on_fs, u32 cmd, u32 id, u64 addr,
                    struct vfs_node *quota_file, int path_err, int readonly)
{
	void *handle = node_handle(on_fs);
	void *file = 0;

	if (!handle)
		return -ENOSYS;
	if (readonly && lkpi_bridge_quotactl_cmd_writes(cmd))
		return -EROFS;
	if (quota_file) {
		file = node_handle(quota_file);
		if (!file)
			path_err = -EXDEV; /* not on an imported filesystem at all */
	}
	return lkpi_bridge_quotactl(handle, cmd, id, (void *)(usize)addr, file,
	                            path_err);
}

void lkpifs_quota_sync_all(int type)
{
	for (int i = 0; i < LKPIFS_MAX_MOUNTS; i++) {
		u64 flags;
		void *root;

		spin_lock_irqsave(&g_lkpifs_roots_lock, &flags);
		root = g_lkpifs_roots[i];
		spin_unlock_irqrestore(&g_lkpifs_roots_lock, flags);
		if (root)
			lkpi_bridge_quota_sync(root, type);
	}
}

/* ── the types on offer ─────────────────────────────────────────── */

static struct vfs_node *lkpifs_mount_btrfs(const char *source, u64 flags,
                                           void *data)
{
	(void)data;
	return lkpifs_mount_type("btrfs", source, flags);
}

/* Registered as "btrfs", not as a second opinion beside one: this is the
 * filesystem b1nix mounts when something says btrfs. */
static struct vfs_fs lkpifs_btrfs = {
	.name = "btrfs",
	.mount = lkpifs_mount_btrfs,
	.umount = lkpifs_umount,
};

#if B1NIX_FS_IMPORT_EXT4
static struct vfs_node *lkpifs_mount_ext4(const char *source, u64 flags,
                                          void *data)
{
	(void)data;
	return lkpifs_mount_type("ext4", source, flags);
}

/* ext4 is also b1nix's ext3 and ext2, as it is Linux's: one driver reads all
 * three formats, registered under each name so a mount shows the one asked. */
static struct vfs_fs lkpifs_ext_types[] = {
	{ .name = "ext4", .mount = lkpifs_mount_ext4, .umount = lkpifs_umount },
	{ .name = "ext3", .mount = lkpifs_mount_ext4, .umount = lkpifs_umount },
	{ .name = "ext2", .mount = lkpifs_mount_ext4, .umount = lkpifs_umount },
};
#endif

void lkpifs_init(void)
{
	vfs_register_fs(&lkpifs_btrfs);
	klog_info("lkpifs: btrfs registered (imported Linux " LKPI_FS_LINUX_VERSION " btrfs)");
#if B1NIX_FS_IMPORT_EXT4
	for (usize i = 0; i < sizeof(lkpifs_ext_types) / sizeof(lkpifs_ext_types[0]); i++)
		vfs_register_fs(&lkpifs_ext_types[i]);
	klog_info("lkpifs: ext4/ext3/ext2 registered (imported Linux " LKPI_FS_LINUX_VERSION " ext4)");
#endif
}

/* ── the proof ──────────────────────────────────────────────────── */

/*
 * Mount through b1nix's own VFS and use the result as any program would.
 *
 * The self-test beside the import (kernel/lkpi/fs_mount_test.c) drives the
 * imported filesystem directly; this one goes the whole way round — mount(2)'s
 * path, then open, read, write, readdir, symlink and unlink by PATH — which is
 * the only thing that shows the bridge works rather than the filesystem.
 */
static int lkpifs_expect_at(const char *dir, const char *name, const char *want);

static int lkpifs_expect_file(const char *path, const char *want)
{
	char buf[128];
	int fd = vfs_open(path);
	isize n;

	if (fd < 0)
		return 0;
	n = vfs_read(fd, buf, sizeof(buf) - 1);
	vfs_close(fd);
	if (n < 0)
		return 0;
	buf[n] = '\0';
	return strcmp(buf, want) == 0;
}

void lkpifs_selftest_type(const char *dev_name, const char *fstype,
                          const char *mnt);

void lkpifs_selftest(const char *dev_name)
{
	lkpifs_selftest_type(dev_name, "btrfs", "/mnt/lkpi");
}

/* `dir/name`, since the mount point is a parameter now. */
static int lkpifs_expect_at(const char *dir, const char *name, const char *want)
{
	char path[128];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return lkpifs_expect_file(path, want);
}

void lkpifs_selftest_type(const char *dev_name, const char *fstype,
                          const char *mnt)
{
	char line[160];
	int rc;

	console_write("LKPI-BRIDGE: start\n");
	vfs_mkdir("/mnt", 0755);   /* may already exist */
	if (vfs_mkdir(mnt, 0755) < 0) {
		struct vfs_node *existing = vfs_find_node(mnt);

		if (IS_ERR(existing) || !existing) {
			console_write("LKPI-BRIDGE: FAIL mkdir\n");
			return;
		}
		vfs_node_put(existing);
	}
	rc = vfs_mount(dev_name, mnt, fstype, 0);
	if (rc < 0) {
		snprintf(line, sizeof(line), "LKPI-BRIDGE: FAIL mount rc=%d\n", rc);
		console_write(line);
		return;
	}
	console_write("LKPI-BRIDGE: ok mount\n");

	/* Read what the host put there, by path. */
	if (lkpifs_expect_at(mnt, "hello.txt", "hello from btrfs\n"))
		console_write("LKPI-BRIDGE: ok read\n");
	else
		console_write("LKPI-BRIDGE: FAIL read\n");

	if (lkpifs_expect_at(mnt, "dir/sub/deep.txt", "nested\n"))
		console_write("LKPI-BRIDGE: ok nested\n");
	else
		console_write("LKPI-BRIDGE: FAIL nested\n");

	/* List the root: the entries come from the imported filesystem. */
	{
		int fd = vfs_open(mnt);
		struct dirent ents[16];
		isize n = fd >= 0 ? vfs_getdents(fd, ents, 16) : -1;
		int saw_hello = 0, saw_big = 0, saw_dir = 0;

		for (isize i = 0; i < n; i++) {
			if (strcmp(ents[i].name, "hello.txt") == 0)
				saw_hello = 1;
			else if (strcmp(ents[i].name, "big.bin") == 0)
				saw_big = 1;
			else if (strcmp(ents[i].name, "dir") == 0)
				saw_dir = 1;
		}
		if (fd >= 0)
			vfs_close(fd);
		if (saw_hello && saw_big && saw_dir) {
			snprintf(line, sizeof(line), "LKPI-BRIDGE: ok readdir entries=%d\n",
			         (int)n);
			console_write(line);
		} else {
			snprintf(line, sizeof(line),
			         "LKPI-BRIDGE: FAIL readdir n=%d hello=%d big=%d dir=%d\n",
			         (int)n, saw_hello, saw_big, saw_dir);
			console_write(line);
		}
	}

	/* A file with real extents, read past its first page. Each record is
	 * fifteen bytes, so the one at 15*8000 holds 8000. */
	{
		char bigpath[128];
		int fd;

		snprintf(bigpath, sizeof(bigpath), "%s/big.bin", mnt);
		fd = vfs_open(bigpath);
		char buf[16];
		isize n = -1;

		if (fd >= 0) {
			vfs_lseek(fd, 15 * 8000, 0 /* SEEK_SET */);
			n = vfs_read(fd, buf, 15);
			vfs_close(fd);
		}
		if (n == 15 && memcmp(buf, "00000000008000\n", 15) == 0)
			console_write("LKPI-BRIDGE: ok extent-read\n");
		else
			console_write("LKPI-BRIDGE: FAIL extent-read\n");
	}

	/* Write, through the same path a program would take. */
	{
		static const char payload[] = "written through the bridge\n";
		int fd;
		isize n = -1;

		char wpath[128];

		snprintf(wpath, sizeof(wpath), "%s/bridge.txt", mnt);
		rc = vfs_create(wpath, 0644);
		fd = rc == 0 ? vfs_open(wpath) : -1;
		if (fd >= 0) {
			n = vfs_write(fd, payload, sizeof(payload) - 1);
			vfs_close(fd);
		}
		if (n == (isize)sizeof(payload) - 1 &&
		    lkpifs_expect_file(wpath, payload))
			console_write("LKPI-BRIDGE: ok write\n");
		else {
			snprintf(line, sizeof(line),
			         "LKPI-BRIDGE: FAIL write rc=%d n=%d\n", rc, (int)n);
			console_write(line);
		}
	}

	/* Extended attributes, through the same calls a program makes.
	 *
	 * The first half reads an attribute the HOST wrote into the image, which
	 * is what proves the imported filesystem is reading the on-disk format
	 * rather than something b1nix stored in memory; the second half writes
	 * one and leaves it there for the host to find. */
	{
		char xpath[128];
		char val[64];
		char names[256];
		isize n;
		int ok = 1;

		snprintf(xpath, sizeof(xpath), "%s/hello.txt", mnt);
		n = vfs_getxattr(xpath, "user.origin", val, sizeof(val), 0);
		if (n != 4 || memcmp(val, "mkfs", 4) != 0)
			ok = 0;

		n = vfs_listxattr(xpath, names, sizeof(names), 0);
		if (n <= 0 || !lkpifs_list_has(names, (usize)n, "user.origin"))
			ok = 0;

		if (ok && vfs_setxattr(xpath, "user.b1nix", "written", 7, 0, 0) != 0)
			ok = 0;
		n = vfs_getxattr(xpath, "user.b1nix", val, sizeof(val), 0);
		if (n != 7 || memcmp(val, "written", 7) != 0)
			ok = 0;

		/* And one that is written and taken away again: a remove that only
		 * forgets the name in memory would still answer this from the disk. */
		if (ok && vfs_setxattr(xpath, "user.gone", "x", 1, 0, 0) == 0 &&
		    vfs_removexattr(xpath, "user.gone", 0) == 0 &&
		    vfs_getxattr(xpath, "user.gone", val, sizeof(val), 0) != -ENODATA)
			ok = 0;

		if (ok)
			console_write("LKPI-BRIDGE: ok xattr\n");
		else {
			snprintf(line, sizeof(line), "LKPI-BRIDGE: FAIL xattr n=%d\n",
			         (int)n);
			console_write(line);
		}
	}

	/* And the namespace: a directory, a rename and an unlink. */
	{
		char dpath[128], mpath[160], spath[128];

		snprintf(dpath, sizeof(dpath), "%s/bridge-dir", mnt);
		snprintf(mpath, sizeof(mpath), "%s/bridge-dir/moved.txt", mnt);
		snprintf(spath, sizeof(spath), "%s/bridge.txt", mnt);
		if (vfs_mkdir(dpath, 0755) == 0 && vfs_rename(spath, mpath) == 0 &&
		    lkpifs_expect_file(mpath, "written through the bridge\n") &&
		    vfs_unlink(mpath) == 0)
			console_write("LKPI-BRIDGE: ok namespace\n");
		else
			console_write("LKPI-BRIDGE: FAIL namespace\n");
	}

	rc = vfs_umount(mnt);
	if (rc == 0)
		console_write("LKPI-BRIDGE: ok umount\n");
	else {
		snprintf(line, sizeof(line), "LKPI-BRIDGE: FAIL umount rc=%d\n", rc);
		console_write(line);
	}
	console_write("LKPI-BRIDGE: done\n");
}
