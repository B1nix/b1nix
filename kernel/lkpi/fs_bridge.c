/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The bridge, Linux side: one imported filesystem, addressed by handle.
 *
 * b1nix's VFS and Linux's cannot meet in one translation unit — `struct inode`,
 * `spinlock_t`, `current` and `kmalloc` all mean different things on the two
 * sides, which is the boundary <lkpi/env.h> describes. So the bridge is two
 * files: this one performs the Linux operations and hands back opaque handles,
 * and kernel/fs/lkpifs.c turns those handles into b1nix VFS nodes.
 *
 * Everything crossing the boundary here is a plain C type. A handle is a
 * `struct dentry *`, which is what a Linux filesystem is addressed by, but the
 * other side never dereferences it.
 */

#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/fileattr.h>
#include <linux/statfs.h>
#include <linux/fs_context.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/xattr.h>
#include <linux/err.h>
#include <linux/printk.h>
#include <lkpi/env.h>

#include "fs_bridge.h"

struct file_system_type *get_fs_type(const char *name);

/* ── the mount this side holds ───────────────────────────────────── */

/*
 * A mounted filesystem as the imported code expects to see it.
 *
 * b1nix's VFS has its own mount objects and knows nothing about these, but a
 * `struct file` handed to the filesystem carries a vfsmount and the filesystem
 * reads it: ext4 records where it was last mounted by rendering the path of
 * `file->f_path.mnt->mnt_root`, and a NULL mnt there faults on the first open.
 * One per superblock, hung off s_mounts, which is where upstream keeps the
 * mounts of a superblock too.
 */
struct bridge_mount {
	struct vfsmount mnt;
	struct list_head sb_link;
};

static struct vfsmount *bridge_mnt_of(struct super_block *sb)
{
	struct bridge_mount *bm;

	if (!sb || list_empty(&sb->s_mounts))
		return NULL;
	bm = list_first_entry(&sb->s_mounts, struct bridge_mount, sb_link);
	return &bm->mnt;
}

/* ── opening a file over a dentry ────────────────────────────────── */

/*
 * A `struct file` built by hand.
 *
 * There is no descriptor table on this side of the boundary — b1nix owns
 * those — so a read or a write assembles the file, uses it and takes it apart
 * again. ->open is not optional: btrfs's readdir builds its entries in a
 * buffer that btrfs_opendir() allocates and hangs off file->private_data.
 */
static int bridge_open(struct file *f, struct dentry *dentry, int write)
{
	memset(f, 0, sizeof(*f));
	f->f_inode = dentry->d_inode;
	f->f_mapping = dentry->d_inode->i_mapping;
	f->f_op = dentry->d_inode->i_fop;
	f->f_path.dentry = dentry;
	f->f_path.mnt = bridge_mnt_of(dentry->d_sb);
	f->f_mode = write ? (FMODE_READ | FMODE_WRITE) : FMODE_READ;
	f->f_flags = write ? O_RDWR : O_RDONLY;
	atomic_long_set(&f->f_count, 1);
	file_ra_state_init(&f->f_ra, f->f_mapping);
	if (f->f_op && f->f_op->open)
		return f->f_op->open(f->f_inode, f);
	return 0;
}

static void bridge_close(struct file *f)
{
	if (f->f_op && f->f_op->release)
		f->f_op->release(f->f_inode, f);
}

/* ── mounting ───────────────────────────────────────────────────── */

/* Why the last mount attempt failed, for the caller's message: the bridge can
 * only answer NULL, and "invalid argument" reads like a corrupt filesystem
 * whatever the real reason was. */
int lkpi_bridge_last_mount_error;

void *lkpi_bridge_mount(const char *fstype, const char *source,
                        unsigned long flags)
{
	struct file_system_type *type = get_fs_type(fstype);
	struct dentry *root;

	if (!type)
		return NULL;
	if (type->mount) {
		/* The old interface, which btrfs still uses in 6.6. */
		root = type->mount(type, (int)flags, source, NULL);
	} else if (type->init_fs_context) {
		/*
		 * The new one, which ext4 uses: the filesystem builds a context,
		 * the context's get_tree produces the superblock, and the root is
		 * left in fc.root. There are no mount options to feed in here — a
		 * parameter would go through ->parse_param before get_tree.
		 */
		struct fs_context fc;
		int err;

		memset(&fc, 0, sizeof(fc));
		fc.fs_type = type;
		fc.purpose = FS_CONTEXT_FOR_MOUNT;
		fc.sb_flags = (unsigned int)flags;
		/* Heap-owned, as upstream's is: btrfs hands it to a duplicate context
		 * whose put frees it. */
		fc.source = source ? kstrdup(source, GFP_KERNEL) : NULL;
		fc.user_ns = &init_user_ns;
		err = type->init_fs_context(&fc);
		if (!err && fc.ops && fc.ops->get_tree)
			err = fc.ops->get_tree(&fc);
		else if (!err)
			err = -EINVAL;
		root = err ? ERR_PTR(err) : fc.root;
		if (fc.ops && fc.ops->free)
			fc.ops->free(&fc);
		kfree(fc.source);
	} else {
		return NULL;
	}
	if (IS_ERR(root) || !root || !root->d_inode) {
		/* Say why: the caller can only report EINVAL, and a mount that fails
		 * for want of the device reads exactly like a corrupt filesystem. */
		lkpi_bridge_last_mount_error =
		    IS_ERR(root) ? (int)PTR_ERR(root) : (root ? -ENOENT : -EIO);
		return NULL;
	}
	{
		struct bridge_mount *bm = kzalloc(sizeof(*bm), GFP_KERNEL);

		if (!bm) {
			dput(root);
			return NULL;
		}
		bm->mnt.mnt_sb = root->d_sb;
		bm->mnt.mnt_root = root;
		bm->mnt.mnt_flags = (int)flags;
		INIT_LIST_HEAD(&bm->sb_link);
		list_add(&bm->sb_link, &root->d_sb->s_mounts);
	}
	return root;
}

void lkpi_bridge_unmount(void *rootp)
{
	struct dentry *root = rootp;
	struct super_block *sb;

	if (!root)
		return;
	sb = root->d_sb;
	if (sb && !list_empty(&sb->s_mounts)) {
		struct bridge_mount *bm =
			list_first_entry(&sb->s_mounts, struct bridge_mount, sb_link);

		list_del(&bm->sb_link);
		kfree(bm);
	}
	dput(root);
	deactivate_super(sb);
}

/* ── names ──────────────────────────────────────────────────────── */

void *lkpi_bridge_lookup(void *dirp, const char *name)
{
	struct dentry *dir = dirp;
	struct dentry *found;

	if (!dir || !dir->d_inode || !name)
		return NULL;
	found = lookup_one_len(name, dir, (int)strlen(name));
	if (IS_ERR(found))
		return NULL;
	if (!found->d_inode) {
		/* A negative dentry is "no such name", not a file. */
		dput(found);
		return NULL;
	}
	return found;
}

void lkpi_bridge_put(void *nodep)
{
	if (nodep)
		dput((struct dentry *)nodep);
}

int lkpi_bridge_attr(void *nodep, struct lkpi_bridge_attr *out)
{
	struct dentry *d = nodep;
	struct inode *inode;

	if (!d || !d->d_inode || !out)
		return -EINVAL;
	inode = d->d_inode;
	out->ino = inode->i_ino;
	out->size = (unsigned long long)i_size_read(inode);
	out->mode = inode->i_mode;
	out->nlink = inode->i_nlink;
	out->uid = __kuid_val(inode->i_uid);
	out->gid = __kgid_val(inode->i_gid);
	out->atime = (unsigned long long)inode->i_atime.tv_sec;
	out->mtime = (unsigned long long)inode->i_mtime.tv_sec;
	out->ctime = (unsigned long long)inode_get_ctime(inode).tv_sec;
	out->atime_nsec = (unsigned int)inode->i_atime.tv_nsec;
	out->mtime_nsec = (unsigned int)inode->i_mtime.tv_nsec;
	out->ctime_nsec = (unsigned int)inode_get_ctime(inode).tv_nsec;
	out->blocks = (unsigned long long)inode->i_blocks;
	out->flags = 0;
	if (inode->i_op && inode->i_op->fileattr_get) {
		struct file_kattr fa;

		memset(&fa, 0, sizeof(fa));
		if (inode->i_op->fileattr_get(d, &fa) == 0)
			out->flags = fa.flags;
	}
	return 0;
}

int lkpi_bridge_set_flags(void *nodep, unsigned int flags)
{
	struct dentry *d = nodep;
	struct inode *inode;
	struct file_kattr fa;
	int ret;

	if (!d || !d->d_inode)
		return -EINVAL;
	inode = d->d_inode;
	if (!inode->i_op || !inode->i_op->fileattr_set)
		return -EOPNOTSUPP;
	fileattr_fill_flags(&fa, flags);
	/* vfs_fileattr_set holds the inode lock around the call. */
	inode_lock(inode);
	ret = inode->i_op->fileattr_set(&nop_mnt_idmap, d, &fa);
	inode_unlock(inode);
	return ret;
}

/* ── data ───────────────────────────────────────────────────────── */

long lkpi_bridge_read(void *nodep, unsigned long long off, char *buf,
                      unsigned long len)
{
	struct dentry *d = nodep;
	struct file f;
	struct kiocb kiocb;
	struct kvec kvec;
	struct iov_iter iter;
	long ret;

	if (!d || !d->d_inode || !buf)
		return -EINVAL;
	ret = bridge_open(&f, d, 0);
	if (ret)
		return ret;
	if (!f.f_op || !f.f_op->read_iter) {
		bridge_close(&f);
		return -EINVAL;
	}
	kvec.iov_base = buf;
	kvec.iov_len = len;
	init_sync_kiocb(&kiocb, &f);
	kiocb.ki_pos = (loff_t)off;
	iov_iter_kvec(&iter, ITER_DEST, &kvec, 1, len);
	ret = f.f_op->read_iter(&kiocb, &iter);
	bridge_close(&f);
	return ret;
}

long lkpi_bridge_write(void *nodep, unsigned long long off, const char *buf,
                       unsigned long len)
{
	struct dentry *d = nodep;
	struct file f;
	struct kiocb kiocb;
	struct kvec kvec;
	struct iov_iter iter;
	long ret;

	if (!d || !d->d_inode || !buf)
		return -EINVAL;
	ret = bridge_open(&f, d, 1);
	if (ret)
		return ret;
	if (!f.f_op || !f.f_op->write_iter) {
		bridge_close(&f);
		return -EINVAL;
	}
	kvec.iov_base = (void *)buf;
	kvec.iov_len = len;
	init_sync_kiocb(&kiocb, &f);
	kiocb.ki_pos = (loff_t)off;
	iov_iter_kvec(&iter, ITER_SOURCE, &kvec, 1, len);
	ret = f.f_op->write_iter(&kiocb, &iter);
	bridge_close(&f);
	return ret;
}

int lkpi_bridge_sync(void *nodep)
{
	struct dentry *d = nodep;
	struct file f;
	int ret;

	if (!d || !d->d_inode)
		return -EINVAL;
	ret = bridge_open(&f, d, 1);
	if (ret)
		return ret;
	if (f.f_op && f.f_op->fsync)
		ret = f.f_op->fsync(&f, 0, LLONG_MAX, 0);
	bridge_close(&f);
	return ret;
}

int lkpi_bridge_sync_fs(void *rootp)
{
	struct dentry *root = rootp;
	struct super_block *sb;

	if (!root)
		return -EINVAL;
	sb = root->d_sb;
	/* In sync(2)'s order: the dirty data first, then the commit. btrfs keeps
	 * written data as delalloc until writeback, and a commit alone put a
	 * file's size on the disk with nothing in its extents. */
	sync_inodes_sb(sb);
	if (sb->s_op && sb->s_op->sync_fs)
		return sb->s_op->sync_fs(sb, 1);
	return 0;
}

/* ── directories ────────────────────────────────────────────────── */

struct bridge_dir_ctx {
	struct dir_context ctx;
	lkpi_bridge_emit_fn emit;
	void *arg;
	int stopped;
};

static bool bridge_actor(struct dir_context *ctx, const char *name, int len,
                         loff_t off, u64 ino, unsigned type)
{
	struct bridge_dir_ctx *p = (struct bridge_dir_ctx *)ctx;

	(void)off;
	if (!p->emit(p->arg, name, len, ino, type)) {
		p->stopped = 1;
		return false;   /* false means STOP, which is upstream's convention */
	}
	return true;
}

int lkpi_bridge_iterate(void *dirp, unsigned long long cookie,
                        unsigned long long *next, lkpi_bridge_emit_fn emit,
                        void *arg)
{
	struct dentry *d = dirp;
	struct bridge_dir_ctx probe;
	struct file f;
	int ret;

	if (!d || !d->d_inode || !emit)
		return -EINVAL;
	memset(&probe, 0, sizeof(probe));
	probe.ctx.actor = bridge_actor;
	probe.ctx.pos = (loff_t)cookie;
	probe.emit = emit;
	probe.arg = arg;

	ret = bridge_open(&f, d, 0);
	if (ret)
		return ret;
	/*
	 * Directory cookies no wider than 32 bits, as Linux gives a 32-bit
	 * caller. ext4's 64-bit htree cookie is (major hash >> 1) << 32 | minor,
	 * which sets bit 62 for half of all names -- the bit the VFS cursor
	 * reserves for "the filesystem's half of the listing is done". Every
	 * getdents batch that ended on such a name ended the listing, and a
	 * large directory lost about half its entries.
	 */
	f.f_mode |= FMODE_32BITHASH;
	if (!f.f_op || !f.f_op->iterate_shared) {
		bridge_close(&f);
		return -ENOTDIR;
	}
	/*
	 * Held shared, as iterate_dir() holds it: btrfs's readdir drops it,
	 * retakes it exclusive and downgrades back, so without it the rwsem's
	 * count went wrong and the next opendir of the directory waited forever.
	 */
	inode_lock_shared(d->d_inode);
	ret = f.f_op->iterate_shared(&f, &probe.ctx);
	inode_unlock_shared(d->d_inode);
	bridge_close(&f);
	/*
	 * The position the filesystem left behind is the cookie to resume from.
	 * It is the filesystem's own numbering — for btrfs a directory index —
	 * which is what makes it survive an unlink elsewhere in the directory.
	 */
	if (next)
		*next = (unsigned long long)probe.ctx.pos;
	return ret;
}

/* ── the namespace ──────────────────────────────────────────────── */

/*
 * A name that must NOT exist yet.
 *
 * Every creating operation needs one: the filesystem is handed a negative
 * dentry and fills it in. Returning the existing one instead would make a
 * create overwrite a file.
 */
static struct dentry *bridge_lookup_negative(struct dentry *dir,
                                             const char *name)
{
	struct dentry *d;

	if (!dir || !dir->d_inode || !name)
		return ERR_PTR(-EINVAL);
	d = lookup_one_len(name, dir, (int)strlen(name));
	if (IS_ERR(d))
		return d;
	if (d->d_inode) {
		dput(d);
		return ERR_PTR(-EEXIST);
	}
	return d;
}

int lkpi_bridge_create(void *dirp, const char *name, unsigned int mode)
{
	struct dentry *dir = dirp;
	struct dentry *d;
	int ret;

	if (!dir->d_inode->i_op || !dir->d_inode->i_op->create)
		return -EOPNOTSUPP;
	inode_lock(dir->d_inode);
	d = bridge_lookup_negative(dir, name);
	if (IS_ERR(d)) {
		inode_unlock(dir->d_inode);
		return (int)PTR_ERR(d);
	}
	ret = dir->d_inode->i_op->create(&nop_mnt_idmap, dir->d_inode, d,
	                                 (umode_t)mode, 0);
	dput(d);
	inode_unlock(dir->d_inode);
	return ret;
}

int lkpi_bridge_mkdir(void *dirp, const char *name, unsigned int mode)
{
	struct dentry *dir = dirp;
	struct dentry *d, *de;
	int ret;

	if (!dir->d_inode->i_op || !dir->d_inode->i_op->mkdir)
		return -EOPNOTSUPP;
	inode_lock(dir->d_inode);
	d = bridge_lookup_negative(dir, name);
	if (IS_ERR(d)) {
		inode_unlock(dir->d_inode);
		return (int)PTR_ERR(d);
	}
	/* 6.15+: NULL when `d` was instantiated, an error pointer, or a
	 * different dentry the filesystem used instead — which replaces `d`. */
	de = dir->d_inode->i_op->mkdir(&nop_mnt_idmap, dir->d_inode, d,
	                               (umode_t)mode);
	ret = IS_ERR(de) ? (int)PTR_ERR(de) : 0;
	if (!IS_ERR_OR_NULL(de)) {
		dput(d);
		d = de;
	}
	dput(d);
	inode_unlock(dir->d_inode);
	return ret;
}

/* A special file: mknod(2) is only used for FIFOs today, which carry no
 * device number. */
int lkpi_bridge_mknod(void *dirp, const char *name, unsigned int mode)
{
	struct dentry *dir = dirp;
	struct dentry *d;
	int ret;

	if (!dir->d_inode->i_op || !dir->d_inode->i_op->mknod)
		return -EOPNOTSUPP;
	inode_lock(dir->d_inode);
	d = bridge_lookup_negative(dir, name);
	if (IS_ERR(d)) {
		inode_unlock(dir->d_inode);
		return (int)PTR_ERR(d);
	}
	ret = dir->d_inode->i_op->mknod(&nop_mnt_idmap, dir->d_inode, d,
	                                (umode_t)mode, 0);
	dput(d);
	inode_unlock(dir->d_inode);
	return ret;
}

int lkpi_bridge_symlink(void *dirp, const char *name, const char *target)
{
	struct dentry *dir = dirp;
	struct dentry *d;
	int ret;

	if (!dir->d_inode->i_op || !dir->d_inode->i_op->symlink)
		return -EOPNOTSUPP;
	inode_lock(dir->d_inode);
	d = bridge_lookup_negative(dir, name);
	if (IS_ERR(d)) {
		inode_unlock(dir->d_inode);
		return (int)PTR_ERR(d);
	}
	ret = dir->d_inode->i_op->symlink(&nop_mnt_idmap, dir->d_inode, d, target);
	dput(d);
	inode_unlock(dir->d_inode);
	return ret;
}

int lkpi_bridge_link(void *dirp, const char *name, void *targetp)
{
	struct dentry *dir = dirp;
	struct dentry *target = targetp;
	struct dentry *d;
	int ret;

	if (!dir->d_inode->i_op || !dir->d_inode->i_op->link)
		return -EOPNOTSUPP;
	if (!target || !target->d_inode)
		return -ENOENT;
	inode_lock(dir->d_inode);
	d = bridge_lookup_negative(dir, name);
	if (IS_ERR(d)) {
		inode_unlock(dir->d_inode);
		return (int)PTR_ERR(d);
	}
	ret = dir->d_inode->i_op->link(target, dir->d_inode, d);
	dput(d);
	inode_unlock(dir->d_inode);
	return ret;
}

/* A name that must exist, for the removing operations. */
static struct dentry *bridge_lookup_positive(struct dentry *dir,
                                             const char *name)
{
	struct dentry *d;

	if (!dir || !dir->d_inode || !name)
		return ERR_PTR(-EINVAL);
	d = lookup_one_len(name, dir, (int)strlen(name));
	if (IS_ERR(d))
		return d;
	if (!d->d_inode) {
		dput(d);
		return ERR_PTR(-ENOENT);
	}
	return d;
}

int lkpi_bridge_unlink(void *dirp, const char *name)
{
	struct dentry *dir = dirp;
	struct dentry *d;
	int ret;

	if (!dir->d_inode->i_op || !dir->d_inode->i_op->unlink)
		return -EOPNOTSUPP;
	inode_lock(dir->d_inode);
	d = bridge_lookup_positive(dir, name);
	if (IS_ERR(d)) {
		inode_unlock(dir->d_inode);
		return (int)PTR_ERR(d);
	}
	/* vfs_unlink holds the victim's lock too; ext4's orphan list asserts it. */
	inode_lock_nested(d->d_inode, I_MUTEX_CHILD);
	ret = dir->d_inode->i_op->unlink(dir->d_inode, d);
	inode_unlock(d->d_inode);
	if (ret == 0)
		d_delete(d);
	dput(d);
	inode_unlock(dir->d_inode);
	return ret;
}

int lkpi_bridge_rmdir(void *dirp, const char *name)
{
	struct dentry *dir = dirp;
	struct dentry *d;
	int ret;

	if (!dir->d_inode->i_op || !dir->d_inode->i_op->rmdir)
		return -EOPNOTSUPP;
	inode_lock(dir->d_inode);
	d = bridge_lookup_positive(dir, name);
	if (IS_ERR(d)) {
		inode_unlock(dir->d_inode);
		return (int)PTR_ERR(d);
	}
	inode_lock_nested(d->d_inode, I_MUTEX_CHILD);
	ret = dir->d_inode->i_op->rmdir(dir->d_inode, d);
	inode_unlock(d->d_inode);
	if (ret == 0)
		d_delete(d);
	dput(d);
	inode_unlock(dir->d_inode);
	return ret;
}

/*
 * The directory locks the VFS holds around namespace operations: the
 * filesystems assume them. Two directories are taken in address order.
 */
static void bridge_lock_dirs(struct inode *a, struct inode *b)
{
	if (a == b) {
		inode_lock(a);
		return;
	}
	if (a > b) {
		struct inode *t = a;

		a = b;
		b = t;
	}
	inode_lock(a);
	inode_lock_nested(b, I_MUTEX_PARENT2);
}

static void bridge_unlock_dirs(struct inode *a, struct inode *b)
{
	inode_unlock(a);
	if (b != a)
		inode_unlock(b);
}

int lkpi_bridge_rename(void *olddirp, const char *oldname, void *newdirp,
                       const char *newname)
{
	struct dentry *olddir = olddirp;
	struct dentry *newdir = newdirp;
	struct dentry *from, *to;
	int ret;

	if (!olddir->d_inode->i_op || !olddir->d_inode->i_op->rename)
		return -EOPNOTSUPP;
	bridge_lock_dirs(olddir->d_inode, newdir->d_inode);
	from = bridge_lookup_positive(olddir, oldname);
	if (IS_ERR(from)) {
		ret = (int)PTR_ERR(from);
		goto out;
	}
	to = lookup_one_len(newname, newdir, (int)strlen(newname));
	if (IS_ERR(to)) {
		dput(from);
		ret = (int)PTR_ERR(to);
		goto out;
	}
	/* vfs_rename locks the moved inode and any it replaces. */
	lock_two_nondirectories(from->d_inode, to->d_inode);
	ret = olddir->d_inode->i_op->rename(&nop_mnt_idmap, olddir->d_inode, from,
	                                    newdir->d_inode, to, 0);
	/* As vfs_rename does: the moved dentry takes the new name. Without it the
	 * cache still answered the old name with the moved inode, so the next
	 * create of that name wrote into the file just renamed away. */
	if (ret == 0 && !(olddir->d_sb->s_type->fs_flags & FS_RENAME_DOES_D_MOVE))
		d_move(from, to);
	unlock_two_nondirectories(from->d_inode, to->d_inode);
	dput(to);
	dput(from);
out:
	bridge_unlock_dirs(olddir->d_inode, newdir->d_inode);
	return ret;
}

int lkpi_bridge_readlink(void *nodep, char *buf, unsigned long len)
{
	struct dentry *d = nodep;
	struct inode *inode;
	struct delayed_call done = { 0, 0 };
	const char *target;
	unsigned long n;

	if (!d || !d->d_inode || !buf || len == 0)
		return -EINVAL;
	inode = d->d_inode;
	if (!S_ISLNK(inode->i_mode))
		return -EINVAL;
	if (!inode->i_op || !inode->i_op->get_link)
		return -EOPNOTSUPP;
	target = inode->i_op->get_link(d, inode, &done);
	if (IS_ERR(target) || !target)
		return target ? (int)PTR_ERR(target) : -EIO;
	n = strlen(target);
	if (n > len - 1)
		n = len - 1;
	memcpy(buf, target, n);
	buf[n] = '\0';
	do_delayed_call(&done);
	return (int)n;
}

int lkpi_bridge_truncate(void *nodep, unsigned long long size)
{
	struct dentry *d = nodep;
	struct iattr attr;
	int ret;

	if (!d || !d->d_inode)
		return -EINVAL;
	if (!d->d_inode->i_op || !d->d_inode->i_op->setattr)
		return -EOPNOTSUPP;
	memset(&attr, 0, sizeof(attr));
	attr.ia_valid = ATTR_SIZE;
	attr.ia_size = (loff_t)size;
	inode_lock(d->d_inode);
	ret = d->d_inode->i_op->setattr(&nop_mnt_idmap, d, &attr);
	inode_unlock(d->d_inode);
	return ret;
}

int lkpi_bridge_chmod(void *nodep, unsigned int mode)
{
	struct dentry *d = nodep;
	struct iattr attr;
	int ret;

	if (!d || !d->d_inode)
		return -EINVAL;
	if (!d->d_inode->i_op || !d->d_inode->i_op->setattr)
		return -EOPNOTSUPP;
	memset(&attr, 0, sizeof(attr));
	attr.ia_valid = ATTR_MODE;
	attr.ia_mode = (umode_t)((mode & 07777) | (d->d_inode->i_mode & S_IFMT));
	inode_lock(d->d_inode);
	ret = d->d_inode->i_op->setattr(&nop_mnt_idmap, d, &attr);
	inode_unlock(d->d_inode);
	return ret;
}

int lkpi_bridge_setattr(void *nodep, unsigned int mode, unsigned int uid,
                        unsigned int gid, unsigned long long atime,
                        unsigned long long mtime)
{
	struct dentry *d = nodep;
	struct inode *inode;
	struct iattr attr;
	int ret = 0;

	if (!d || !d->d_inode)
		return -EINVAL;
	inode = d->d_inode;
	if (!inode->i_op || !inode->i_op->setattr)
		return -EOPNOTSUPP;
	memset(&attr, 0, sizeof(attr));
	if ((inode->i_mode & 07777) != (mode & 07777)) {
		attr.ia_valid |= ATTR_MODE;
		attr.ia_mode = (umode_t)((mode & 07777) | (inode->i_mode & S_IFMT));
	}
	if (__kuid_val(inode->i_uid) != uid) {
		attr.ia_valid |= ATTR_UID;
		attr.ia_uid = KUIDT_INIT(uid);
	}
	if (__kgid_val(inode->i_gid) != gid) {
		attr.ia_valid |= ATTR_GID;
		attr.ia_gid = KGIDT_INIT(gid);
	}
	if ((unsigned long long)inode->i_atime.tv_sec != atime) {
		attr.ia_valid |= ATTR_ATIME | ATTR_ATIME_SET;
		attr.ia_atime.tv_sec = (time64_t)atime;
	}
	if ((unsigned long long)inode->i_mtime.tv_sec != mtime) {
		attr.ia_valid |= ATTR_MTIME | ATTR_MTIME_SET;
		attr.ia_mtime.tv_sec = (time64_t)mtime;
	}
	if (!attr.ia_valid)
		return 0;
	attr.ia_valid |= ATTR_CTIME;
	attr.ia_ctime = current_time(inode);
	inode_lock(inode);
	ret = inode->i_op->setattr(&nop_mnt_idmap, d, &attr);
	inode_unlock(inode);
	return ret;
}

/* FITRIM. The ioctl copies its range from user memory, so the filesystems'
 * own trim entry points are called with a kernel copy instead. */
/* ext4 is in the link only with B1NIX_FS_IMPORT=1, a flag these objects are
 * not built with: a weak reference resolves to NULL without it. */
int ext4_trim_fs(struct super_block *sb, struct fstrim_range *range) __attribute__((weak));
struct btrfs_fs_info;
int btrfs_trim_fs(struct btrfs_fs_info *fs_info, struct fstrim_range *range);

int lkpi_bridge_fitrim(void *nodep, unsigned long long start,
                       unsigned long long len, unsigned long long minlen,
                       unsigned long long *trimmed)
{
	struct dentry *d = nodep;
	struct super_block *sb = d ? d->d_sb : NULL;
	struct fstrim_range range = {.start = start, .len = len, .minlen = minlen};
	int ret = -EOPNOTSUPP;

	if (!sb || !sb->s_type || !sb->s_bdev || !trimmed)
		return -EINVAL;
	if (!bdev_max_discard_sectors(sb->s_bdev))
		return -EOPNOTSUPP;
	if (sb_rdonly(sb))
		return -EROFS;
	if (strcmp(sb->s_type->name, "btrfs") == 0)
		ret = btrfs_trim_fs(sb->s_fs_info, &range);
	else if (strcmp(sb->s_type->name, "ext4") == 0 && ext4_trim_fs)
		ret = ext4_trim_fs(sb, &range);
	if (ret < 0)
		return ret;
	*trimmed = range.len;
	return 0;
}

int lkpi_bridge_statfs(void *nodep, struct lkpi_bridge_statfs *out)
{
	struct dentry *d = nodep;
	struct kstatfs st;
	int ret;

	if (!d || !d->d_sb || !d->d_sb->s_op || !d->d_sb->s_op->statfs || !out)
		return -EOPNOTSUPP;
	memset(&st, 0, sizeof(st));
	ret = d->d_sb->s_op->statfs(d, &st);
	if (ret)
		return ret;
	out->type = (unsigned long long)st.f_type;
	out->bsize = (unsigned long long)st.f_bsize;
	out->blocks = st.f_blocks;
	out->bfree = st.f_bfree;
	out->bavail = st.f_bavail;
	out->files = st.f_files;
	out->ffree = st.f_ffree;
	out->fsid = ((unsigned long long)(u32)st.f_fsid.val[1] << 32) |
	            (u32)st.f_fsid.val[0];
	out->namelen = (unsigned long long)st.f_namelen;
	out->frsize = (unsigned long long)(st.f_frsize ? st.f_frsize : st.f_bsize);
	out->flags = (unsigned long long)st.f_flags;
	return 0;
}

/* ── extended attributes ────────────────────────────────────────── */

/*
 * The imported filesystems store these themselves — ext4 in the inode's extra
 * space and an overflow block, btrfs as items in its tree — so everything here
 * is dispatch: find the handler that owns the name's prefix, or ask the inode
 * to list what it has.
 */
long lkpi_bridge_getxattr(void *nodep, const char *name, void *value,
                          unsigned long size)
{
	struct dentry *d = nodep;

	if (!d || !d->d_inode || !name)
		return -EINVAL;
	return __vfs_getxattr(d, d->d_inode, name, value, (size_t)size);
}

int lkpi_bridge_setxattr(void *nodep, const char *name, const void *value,
                         unsigned long size, int flags)
{
	struct dentry *d = nodep;

	if (!d || !d->d_inode || !name)
		return -EINVAL;
	return __vfs_setxattr(&nop_mnt_idmap, d, d->d_inode, name, value,
	                      (size_t)size, flags);
}

int lkpi_bridge_removexattr(void *nodep, const char *name)
{
	struct dentry *d = nodep;

	if (!d || !d->d_inode || !name)
		return -EINVAL;
	/* A NULL value with size 0 is how upstream spells removal: the handler
	 * sees the same call it would for a set and deletes instead. */
	return __vfs_setxattr(&nop_mnt_idmap, d, d->d_inode, name, NULL, 0,
	                      XATTR_REPLACE);
}

long lkpi_bridge_listxattr(void *nodep, char *list, unsigned long size)
{
	struct dentry *d = nodep;

	if (!d || !d->d_inode)
		return -EINVAL;
	if (!d->d_inode->i_op || !d->d_inode->i_op->listxattr)
		return -EOPNOTSUPP;
	return d->d_inode->i_op->listxattr(d, list, (size_t)size);
}
