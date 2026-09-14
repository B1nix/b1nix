/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the superblock, and mounting.
 *
 * A superblock is one mounted filesystem: its device, its block size, its root,
 * and the operations that reach the rest. This file allocates them, hands them
 * to a filesystem's `fill_super`, and takes them apart again.
 *
 * The mount path a filesystem takes here is the block-device one:
 *
 *   register_filesystem      the type becomes known
 *   get_tree_bdev            open the device, allocate a superblock, call
 *                            fill_super, and publish the root
 *   deactivate_locked_super  the last reference: put_super, then the device
 *
 * `s_umount` is held across the whole of that. It is the lock that makes a
 * superblock's construction atomic with respect to anything that could find it
 * half-built — and it is taken WRITE for the mount and the unmount, which is
 * why a filesystem that takes it again inside fill_super deadlocks.
 */

#include <linux/fs.h>
#include <linux/string.h>
#include <linux/fs_context.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/writeback.h>
#include <linux/pagemap.h>
#include <lkpi/env.h>

/* ── the registry ───────────────────────────────────────────────── */

static struct file_system_type *file_systems;
static spinlock_t fs_type_lock;
static int fs_type_lock_ready;

static void fs_type_lock_init(void)
{
	if (!fs_type_lock_ready) {
		spin_lock_init(&fs_type_lock);
		fs_type_lock_ready = 1;
	}
}

int register_filesystem(struct file_system_type *fs)
{
	struct file_system_type **p;
	unsigned long flags;

	fs_type_lock_init();
	spin_lock_irqsave(&fs_type_lock, flags);
	for (p = &file_systems; *p; p = &(*p)->next) {
		if (*p == fs || !strcmp((*p)->name, fs->name)) {
			spin_unlock_irqrestore(&fs_type_lock, flags);
			return -EBUSY;
		}
	}
	fs->next = NULL;
	*p = fs;
	spin_unlock_irqrestore(&fs_type_lock, flags);
	return 0;
}

int unregister_filesystem(struct file_system_type *fs)
{
	struct file_system_type **p;
	unsigned long flags;

	fs_type_lock_init();
	spin_lock_irqsave(&fs_type_lock, flags);
	for (p = &file_systems; *p; p = &(*p)->next) {
		if (*p == fs) {
			*p = fs->next;
			fs->next = NULL;
			spin_unlock_irqrestore(&fs_type_lock, flags);
			return 0;
		}
	}
	spin_unlock_irqrestore(&fs_type_lock, flags);
	return -EINVAL;
}

struct file_system_type *get_fs_type(const char *name)
{
	struct file_system_type *fs;
	unsigned long flags;

	fs_type_lock_init();
	spin_lock_irqsave(&fs_type_lock, flags);
	for (fs = file_systems; fs; fs = fs->next)
		if (!strcmp(fs->name, name))
			break;
	spin_unlock_irqrestore(&fs_type_lock, flags);
	return fs;
}

/*
 * The registered types, for the bridge.
 *
 * b1nix's VFS needs to know which filesystems the imported code registered, and
 * this is the only place that knows. Exposed as an iterator rather than the
 * list head so the lock stays here.
 */
struct file_system_type *lkpi_fs_type_first(void)
{
	return file_systems;
}

struct file_system_type *lkpi_fs_type_next(struct file_system_type *fs)
{
	return fs ? fs->next : NULL;
}

/* ── anonymous device numbers ───────────────────────────────────── */

/*
 * A filesystem with no block device of its own still needs a device number:
 * `stat` reports one, and two files are the same file only if their (dev, ino)
 * agree. They come from a small pool, and the free is not optional — leaking
 * one leaks a minor for the life of the boot.
 */
#define LKPI_ANON_DEV_FIRST 1
#define LKPI_ANON_DEV_COUNT 256

static unsigned long anon_dev_map[LKPI_ANON_DEV_COUNT / (8 * sizeof(long))];
static spinlock_t anon_dev_lock;
static int anon_dev_ready;

int get_anon_bdev(dev_t *p)
{
	unsigned int i;
	unsigned long flags;

	if (!anon_dev_ready) {
		spin_lock_init(&anon_dev_lock);
		anon_dev_ready = 1;
	}
	spin_lock_irqsave(&anon_dev_lock, flags);
	for (i = LKPI_ANON_DEV_FIRST; i < LKPI_ANON_DEV_COUNT; i++) {
		unsigned long *word = &anon_dev_map[i / (8 * sizeof(long))];
		unsigned long bit = 1ul << (i % (8 * sizeof(long)));

		if (!(*word & bit)) {
			*word |= bit;
			spin_unlock_irqrestore(&anon_dev_lock, flags);
			/* Major 0 is what an anonymous device uses, by convention and by
			 * what userspace expects to see in st_dev. */
			*p = (dev_t)i;
			return 0;
		}
	}
	spin_unlock_irqrestore(&anon_dev_lock, flags);
	return -EMFILE;
}

void free_anon_bdev(dev_t dev)
{
	unsigned int i = (unsigned int)dev;
	unsigned long flags;

	if (i >= LKPI_ANON_DEV_COUNT || !anon_dev_ready)
		return;
	spin_lock_irqsave(&anon_dev_lock, flags);
	anon_dev_map[i / (8 * sizeof(long))] &= ~(1ul << (i % (8 * sizeof(long))));
	spin_unlock_irqrestore(&anon_dev_lock, flags);
}

int set_anon_super_fc(struct super_block *sb, struct fs_context *fc);

int set_anon_super(struct super_block *sb, void *data)
{
	(void)data;
	return get_anon_bdev(&sb->s_dev);
}

/* ── allocating a superblock ────────────────────────────────────── */

/*
 * The driver-model device the bdi belongs to.
 *
 * It exists because imported code reaches THROUGH the bdi for a kobject —
 * btrfs links its sysfs directory to `sb->s_bdi->dev->kobj` — and a NULL dev
 * there is not read as "no device": the expression yields a pointer to the
 * kobject member of address zero, which is a kobject the link call then
 * refuses. The mount failed with "failed to init sysfs interface: -22".
 *
 * It carries a name and no sysfs directory, so a link against it succeeds
 * without publishing anything that points nowhere.
 */
static struct device lkpi_bdi_device = {
	.kobj = { .name = "lkpi-fs" },
};

struct backing_dev_info lkpi_default_bdi = {
	.dev = &lkpi_bdi_device,
	/* 32 pages of read-ahead: the same window b1nix's block layer uses by
	 * default. A filesystem that knows better — btrfs, from its stripe
	 * geometry — raises it at mount. */
	.ra_pages = 32,
	.io_pages = 32,
	.name = "lkpi-fs",
};

static struct super_block *alloc_super(struct file_system_type *type, int flags)
{
	struct super_block *sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	int level;

	if (!sb)
		return NULL;

	INIT_LIST_HEAD(&sb->s_list);
	INIT_LIST_HEAD(&sb->s_inodes);
	INIT_LIST_HEAD(&sb->s_inodes_wb);
	INIT_LIST_HEAD(&sb->s_mounts);
	INIT_HLIST_NODE(&sb->s_instances);
	INIT_HLIST_BL_HEAD(&sb->s_roots);
	spin_lock_init(&sb->s_inode_list_lock);
	spin_lock_init(&sb->s_inode_wblist_lock);
	mutex_init(&sb->s_vfs_rename_mutex);
	mutex_init(&sb->s_sync_lock);
	init_rwsem(&sb->s_umount);
	init_rwsem(&sb->s_dquot.dqio_sem);

	for (level = 0; level < SB_FREEZE_LEVELS; level++)
		percpu_init_rwsem(&sb->s_writers.rw_sem[level]);

	sb->s_type = type;
	sb->s_flags = (unsigned long)flags;
	sb->s_count = 1;
	atomic_set(&sb->s_active, 1);
	sb->s_bdi = &lkpi_default_bdi;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;
	/*
	 * The widest range a filesystem can narrow. A filesystem that stores
	 * 32-bit seconds sets its own bounds in fill_super; leaving these
	 * unbounded until then is what lets it, and a default that was already
	 * narrow would silently clamp timestamps it can store.
	 */
	sb->s_time_min = -(time64_t)(1ull << 62);
	sb->s_time_max = (time64_t)(1ull << 62);
	sb->s_blocksize = 4096;
	sb->s_blocksize_bits = 12;
	sb->s_user_ns = &init_user_ns;
	return sb;
}

static void destroy_super(struct super_block *sb)
{
	int level;

	for (level = 0; level < SB_FREEZE_LEVELS; level++)
		percpu_free_rwsem(&sb->s_writers.rw_sem[level]);
	kfree(sb);
}

/*
 * Find an existing superblock or make one.
 *
 * `test` is how a filesystem recognises its own: btrfs compares the device's
 * fs_devices, so that mounting a second subvolume of the same filesystem
 * reuses the superblock rather than reading the disk twice. Returning a fresh
 * superblock where one exists would give one filesystem two of everything.
 */
struct super_block *sget(struct file_system_type *type,
                         int (*test)(struct super_block *, void *),
                         int (*set)(struct super_block *, void *), int flags,
                         void *data)
{
	struct super_block *sb;
	int err;

	if (test) {
		hlist_for_each_entry(sb, &type->fs_supers, s_instances) {
			if (!test(sb, data))
				continue;
			/* Found one: take a reference and hand it back with s_umount
			 * held, which is the state the caller expects from either
			 * branch. */
			atomic_inc(&sb->s_active);
			down_write(&sb->s_umount);
			return sb;
		}
	}

	sb = alloc_super(type, flags);
	if (!sb)
		return ERR_PTR(-ENOMEM);
	if (set) {
		err = set(sb, data);
		if (err) {
			destroy_super(sb);
			return ERR_PTR(err);
		}
	}
	down_write(&sb->s_umount);
	hlist_add_head(&sb->s_instances, &type->fs_supers);
	return sb;
}

struct super_block *sget_fc(struct fs_context *fc,
                            int (*test)(struct super_block *,
                                        struct fs_context *),
                            int (*set)(struct super_block *,
                                       struct fs_context *))
{
	struct super_block *sb;
	int err;

	if (test) {
		hlist_for_each_entry(sb, &fc->fs_type->fs_supers, s_instances) {
			if (!test(sb, fc))
				continue;
			atomic_inc(&sb->s_active);
			down_write(&sb->s_umount);
			return sb;
		}
	}

	sb = alloc_super(fc->fs_type, (int)fc->sb_flags);
	if (!sb)
		return ERR_PTR(-ENOMEM);
	sb->s_fs_info = fc->s_fs_info;
	if (set) {
		err = set(sb, fc);
		if (err) {
			/* Still the context's: its free releases it. */
			sb->s_fs_info = NULL;
			destroy_super(sb);
			return ERR_PTR(err);
		}
	}
	/* The superblock owns it now; a later free of the context must not. */
	fc->s_fs_info = NULL;
	down_write(&sb->s_umount);
	hlist_add_head(&sb->s_instances, &fc->fs_type->fs_supers);
	return sb;
}

/* ── tearing one down ───────────────────────────────────────────── */

void generic_shutdown_super(struct super_block *sb)
{
	if (sb->s_root) {
		/*
		 * The root's reference is the last one holding the tree up; dropping
		 * it collapses the dentries, which drops their inodes, which is what
		 * gives the filesystem its evict_inode calls before put_super.
		 */
		dput(sb->s_root);
		sb->s_root = NULL;
		/* Cached inodes kept past their last reference: write, then evict,
		 * as upstream's sync_filesystem + evict_inodes do here. */
		sync_inodes_sb(sb);
		evict_inodes(sb);
		/*
		 * put_super belongs inside this branch, exactly as upstream has it.
		 * A superblock with no root never finished being filled — its
		 * fill_super failed and has already undone its own work — so calling
		 * put_super there is a second teardown of freed structures. btrfs
		 * died in close_ctree walking a device list open_ctree had emptied.
		 */
		if (sb->s_op && sb->s_op->put_super)
			sb->s_op->put_super(sb);
	}
	sb->s_flags &= ~SB_ACTIVE;
	if (!hlist_unhashed(&sb->s_instances))
		hlist_del_init(&sb->s_instances);
}

void deactivate_locked_super(struct super_block *sb)
{
	if (!atomic_dec_and_test(&sb->s_active)) {
		up_write(&sb->s_umount);
		return;
	}
	if (sb->s_type && sb->s_type->kill_sb)
		sb->s_type->kill_sb(sb);
	else
		generic_shutdown_super(sb);
	up_write(&sb->s_umount);
	destroy_super(sb);
}

void deactivate_super(struct super_block *sb)
{
	down_write(&sb->s_umount);
	deactivate_locked_super(sb);
}

void kill_block_super(struct super_block *sb)
{
	struct block_device *bdev = sb->s_bdev;

	generic_shutdown_super(sb);
	if (bdev) {
		/* The device is released after the filesystem is done with it, never
		 * before: put_super writes the last of the metadata. */
		sync_blockdev(bdev);
		blkdev_put(bdev, sb);
		sb->s_bdev = NULL;
	}
}

void kill_anon_super(struct super_block *sb)
{
	dev_t dev = sb->s_dev;

	generic_shutdown_super(sb);
	free_anon_bdev(dev);
}

/* ── the block-device mount ─────────────────────────────────────── */

const char *lkpi_bdev_printk_name(const void *bdev);

static int set_bdev_super(struct super_block *sb, void *data)
{
	struct block_device *bdev = data;
	const char *name;

	sb->s_bdev = bdev;
	sb->s_dev = bdev->bd_dev;
	sb->s_bdi = &lkpi_default_bdi;
	/* s_id is what a filesystem prints to say WHICH filesystem is speaking —
	 * every ext4_msg and btrfs message opens with it. Left empty it reads as
	 * "EXT4-fs ()", which names nothing. */
	name = lkpi_bdev_printk_name(bdev);
	if (name) {
		strncpy(sb->s_id, name, sizeof(sb->s_id) - 1);
		sb->s_id[sizeof(sb->s_id) - 1] = '\0';
	}
	return 0;
}

static int test_bdev_super(struct super_block *sb, void *data)
{
	return sb->s_bdev == data;
}

/*
 * Open the device, get a superblock for it, and fill it in.
 *
 * The order is what makes a failed mount leave nothing behind: the device is
 * opened first (so a bad path fails before anything is allocated), the
 * superblock is filled second, and only a successful fill publishes a root.
 */
int get_tree_bdev(struct fs_context *fc,
                  int (*fill_super)(struct super_block *sb,
                                    struct fs_context *fc))
{
	struct block_device *bdev;
	struct super_block *sb;
	blk_mode_t mode;
	int err;

	if (!fc->source)
		return -EINVAL;

	mode = sb_open_mode(fc->sb_flags);
	bdev = blkdev_get_by_path(fc->source, mode, fc, &fs_holder_ops);
	if (IS_ERR(bdev))
		return PTR_ERR(bdev);

	sb = sget(fc->fs_type, test_bdev_super, set_bdev_super,
	          (int)fc->sb_flags, bdev);
	if (IS_ERR(sb)) {
		blkdev_put(bdev, fc);
		return PTR_ERR(sb);
	}

	if (sb->s_root) {
		/* Already mounted: this is a second mount of the same filesystem,
		 * and it shares the superblock. The device reference taken above is
		 * one too many. */
		blkdev_put(bdev, fc);
		fc->root = dget(sb->s_root);
		up_write(&sb->s_umount);
		return 0;
	}

	sb->s_mode = mode;
	/* The device's block size is what the filesystem will address it in
	 * until fill_super says otherwise. */
	sb_set_blocksize(sb, (int)bdev_logical_block_size(bdev));

	err = fill_super(sb, fc);
	if (err) {
		deactivate_locked_super(sb);
		return err;
	}

	/*
	 * SB_ACTIVE and SB_BORN are set only now. Before this point the
	 * superblock is half-built, and a filesystem's own error path tests
	 * SB_BORN before trying to report through it.
	 */
	sb->s_flags |= SB_ACTIVE | SB_BORN;
	fc->root = dget(sb->s_root);
	up_write(&sb->s_umount);
	return 0;
}

int get_tree_nodev(struct fs_context *fc,
                   int (*fill_super)(struct super_block *sb,
                                     struct fs_context *fc))
{
	struct super_block *sb;
	int err;

	sb = sget_fc(fc, NULL, set_anon_super_fc);
	if (IS_ERR(sb))
		return PTR_ERR(sb);
	err = fill_super(sb, fc);
	if (err) {
		deactivate_locked_super(sb);
		return err;
	}
	sb->s_flags |= SB_ACTIVE | SB_BORN;
	fc->root = dget(sb->s_root);
	up_write(&sb->s_umount);
	return 0;
}

int set_anon_super_fc(struct super_block *sb, struct fs_context *fc)
{
	(void)fc;
	return get_anon_bdev(&sb->s_dev);
}

int get_tree_keyed(struct fs_context *fc,
                   int (*fill_super)(struct super_block *sb,
                                     struct fs_context *fc),
                   void *key)
{
	fc->s_fs_info = key;
	return get_tree_nodev(fc, fill_super);
}

int get_tree_block_key(struct fs_context *fc,
                       int (*fill_super)(struct super_block *sb,
                                         struct fs_context *fc),
                       void *key)
{
	fc->sget_key = key;
	return get_tree_bdev(fc, fill_super);
}

int get_tree_single(struct fs_context *fc,
                    int (*fill_super)(struct super_block *sb,
                                      struct fs_context *fc))
{
	return get_tree_nodev(fc, fill_super);
}

/*
 * The old mount interface, for a filesystem that has not moved to fs_context.
 * btrfs still uses it.
 */
struct dentry *mount_bdev(struct file_system_type *fs_type, int flags,
                          const char *dev_name, void *data,
                          int (*fill_super)(struct super_block *, void *, int))
{
	struct block_device *bdev;
	struct super_block *sb;
	blk_mode_t mode = sb_open_mode((unsigned int)flags);
	int err;

	bdev = blkdev_get_by_path(dev_name, mode, fs_type, &fs_holder_ops);
	if (IS_ERR(bdev))
		return ERR_CAST(bdev);

	sb = sget(fs_type, test_bdev_super, set_bdev_super, flags, bdev);
	if (IS_ERR(sb)) {
		blkdev_put(bdev, fs_type);
		return ERR_CAST(sb);
	}

	if (sb->s_root) {
		blkdev_put(bdev, fs_type);
		up_write(&sb->s_umount);
		return dget(sb->s_root);
	}

	sb->s_mode = mode;
	sb_set_blocksize(sb, (int)bdev_logical_block_size(bdev));
	err = fill_super(sb, data, (flags & SB_SILENT) ? 1 : 0);
	if (err) {
		deactivate_locked_super(sb);
		return ERR_PTR(err);
	}
	sb->s_flags |= SB_ACTIVE | SB_BORN;
	up_write(&sb->s_umount);
	return dget(sb->s_root);
}

/* ── block sizes ────────────────────────────────────────────────── */

int sb_set_blocksize(struct super_block *sb, int size)
{
	/*
	 * A block size must be a power of two, at least as large as the device's
	 * own, and no larger than a page — the last because a block is read into
	 * one page, and a larger one has nowhere to go.
	 */
	if (size < 512 || size > (int)PAGE_SIZE || (size & (size - 1)))
		return 0;
	if (sb->s_bdev && size < (int)bdev_logical_block_size(sb->s_bdev))
		return 0;
	sb->s_blocksize = (unsigned long)size;
	sb->s_blocksize_bits = (unsigned char)blksize_bits((unsigned int)size);
	if (sb->s_bdev)
		bdev_set_blocksize(sb->s_bdev, size);
	return size;
}

int sb_min_blocksize(struct super_block *sb, int size)
{
	int minsize = sb->s_bdev ? (int)bdev_logical_block_size(sb->s_bdev) : 512;

	if (size < minsize)
		size = minsize;
	return sb_set_blocksize(sb, size);
}

u64 sb_bdev_nr_blocks(struct super_block *sb)
{
	if (!sb->s_bdev)
		return 0;
	return (u64)bdev_nr_sectors(sb->s_bdev) >>
	       (sb->s_blocksize_bits - SECTOR_SHIFT);
}

int super_setup_bdi(struct super_block *sb)
{
	sb->s_bdi = &lkpi_default_bdi;
	return 0;
}

int super_setup_bdi_name(struct super_block *sb, char *fmt, ...)
{
	(void)fmt;
	return super_setup_bdi(sb);
}

/* ── freezing ───────────────────────────────────────────────────── */

/*
 * The write-side references a freeze waits for.
 *
 * `sb_start_write` may sleep and must be paired on every path out, including
 * the error paths — a leaked reference does not fail anything until somebody
 * tries to freeze the filesystem, at which point it hangs.
 */
void sb_start_write(struct super_block *sb)
{
	percpu_down_read(&sb->s_writers.rw_sem[SB_FREEZE_WRITE - 1]);
}

bool sb_start_write_trylock(struct super_block *sb)
{
	return percpu_down_read_trylock(&sb->s_writers.rw_sem[SB_FREEZE_WRITE - 1]);
}

void sb_end_write(struct super_block *sb)
{
	percpu_up_read(&sb->s_writers.rw_sem[SB_FREEZE_WRITE - 1]);
}

void sb_start_pagefault(struct super_block *sb)
{
	percpu_down_read(&sb->s_writers.rw_sem[SB_FREEZE_PAGEFAULT - 1]);
}

void sb_end_pagefault(struct super_block *sb)
{
	percpu_up_read(&sb->s_writers.rw_sem[SB_FREEZE_PAGEFAULT - 1]);
}

void sb_start_intwrite(struct super_block *sb)
{
	percpu_down_read(&sb->s_writers.rw_sem[SB_FREEZE_FS - 1]);
}

bool sb_start_intwrite_trylock(struct super_block *sb)
{
	return percpu_down_read_trylock(&sb->s_writers.rw_sem[SB_FREEZE_FS - 1]);
}

void sb_end_intwrite(struct super_block *sb)
{
	percpu_up_read(&sb->s_writers.rw_sem[SB_FREEZE_FS - 1]);
}

bool sb_write_started(struct super_block *sb)
{
	return percpu_is_read_locked(&sb->s_writers.rw_sem[SB_FREEZE_WRITE - 1]);
}

/* ── syncing ────────────────────────────────────────────────────── */

int sync_filesystem(struct super_block *sb)
{
	int ret = 0;

	if (!sb)
		return 0;
	if (sb->s_op && sb->s_op->sync_fs)
		ret = sb->s_op->sync_fs(sb, 1);
	if (sb->s_bdev) {
		int err = sync_blockdev(sb->s_bdev);

		if (!ret)
			ret = err;
	}
	return ret;
}

void sync_inodes_sb(struct super_block *sb)
{
	struct inode *inode;

	/* Every inode with dirty pages, written through its own mapping. The
	 * walk is the superblock's list rather than a writeback queue, which
	 * makes it proportional to the number of cached inodes — acceptable at
	 * sync time and not something to do on a hot path. */
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list)
		filemap_write_and_wait(inode->i_mapping);
}

void writeback_inodes_sb(struct super_block *sb, enum wb_reason reason)
{
	(void)reason;
	sync_inodes_sb(sb);
}

void try_to_writeback_inodes_sb(struct super_block *sb, enum wb_reason reason)
{
	writeback_inodes_sb(sb, reason);
}

int write_inode_now(struct inode *inode, int sync)
{
	struct writeback_control wbc = {
		.sync_mode = sync ? WB_SYNC_ALL : WB_SYNC_NONE,
		.nr_to_write = LONG_MAX,
	};

	if (inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->write_inode)
		return inode->i_sb->s_op->write_inode(inode, &wbc);
	return 0;
}

int sync_inode_metadata(struct inode *inode, int wait)
{
	return write_inode_now(inode, wait);
}

/* ── mounting from inside the kernel ────────────────────────────── */

/*
 * Mount a filesystem and return the vfsmount.
 *
 * This is on btrfs's main path, not a corner of it: `btrfs_mount` mounts the
 * device's real root through `vfs_kern_mount` and then picks the requested
 * subvolume out of it with `mount_subtree`. Two mounts, one device — which is
 * why the superblock lookup in sget() has to recognise the second as the same
 * filesystem.
 *
 * The vfsmount produced here is a real object with a real root, and b1nix's own
 * VFS knows nothing about it: it exists so the imported code can hold a
 * filesystem it has mounted. The bridge is what will connect one to a path.
 */
struct vfsmount *vfs_kern_mount(struct file_system_type *type, int flags,
                                const char *name, void *data)
{
	struct vfsmount *mnt;
	struct dentry *root;

	if (!type)
		return ERR_PTR(-EINVAL);

	mnt = kzalloc(sizeof(*mnt), GFP_KERNEL);
	if (!mnt)
		return ERR_PTR(-ENOMEM);

	if (type->mount) {
		root = type->mount(type, flags, name, data);
	} else if (type->init_fs_context) {
		struct fs_context fc;
		int err;

		memset(&fc, 0, sizeof(fc));
		fc.fs_type = type;
		fc.purpose = FS_CONTEXT_FOR_MOUNT;
		fc.sb_flags = (unsigned int)flags;
		fc.source = name ? kstrdup(name, GFP_KERNEL) : NULL;
		fc.user_ns = &init_user_ns;
		err = type->init_fs_context(&fc);
		if (!err && fc.ops && fc.ops->get_tree)
			err = fc.ops->get_tree(&fc);
		root = err ? ERR_PTR(err) : fc.root;
		if (fc.ops && fc.ops->free)
			fc.ops->free(&fc);
		kfree(fc.source);
	} else {
		root = ERR_PTR(-EINVAL);
	}

	if (IS_ERR(root)) {
		kfree(mnt);
		return ERR_CAST(root);
	}

	mnt->mnt_root = root;
	mnt->mnt_sb = root->d_sb;
	mnt->mnt_flags = flags;
	/*
	 * Release s_umount, which sget() took and the fill path kept.
	 *
	 * Upstream drops it here for the same reason: everything below has
	 * finished with the superblock, and the next thing anyone does with it —
	 * an unmount — takes the same lock. Leaving it held made the mount
	 * succeed and deactivate_super() then wait on itself forever.
	 */
	up_write(&mnt->mnt_sb->s_umount);
	return mnt;
}

/*
 * Copy a mount context for a second mount of the same filesystem — btrfs
 * mounts the whole filesystem through a duplicate, then picks the subvolume
 * out of that mount. The filesystem's own dup copies its private options; the
 * pointers the copy must not share are cleared first, as upstream does.
 */
struct fs_context *vfs_dup_fs_context(struct fs_context *src_fc)
{
	struct fs_context *fc;
	int ret;

	if (!src_fc->ops || !src_fc->ops->dup)
		return ERR_PTR(-EOPNOTSUPP);
	fc = kmemdup(src_fc, sizeof(*fc), GFP_KERNEL);
	if (!fc)
		return ERR_PTR(-ENOMEM);
	fc->fs_private = NULL;
	fc->s_fs_info = NULL;
	fc->source = NULL;
	fc->security = NULL;
	fc->root = NULL;
	ret = fc->ops->dup(fc, src_fc);
	if (ret < 0) {
		put_fs_context(fc);
		return ERR_PTR(ret);
	}
	fc->need_free = true;
	return fc;
}

/* Release a heap-allocated context: its root, if a get_tree left one, the
 * filesystem's private state, and the context itself. */
void put_fs_context(struct fs_context *fc)
{
	if (fc->root) {
		struct super_block *sb = fc->root->d_sb;

		dput(fc->root);
		fc->root = NULL;
		deactivate_super(sb);
	}
	if (fc->ops && fc->ops->free)
		fc->ops->free(fc);
	kfree(fc->source);
	kfree(fc);
}

/* A vfsmount for the root a get_tree produced. It takes its own references
 * on the root and the superblock, so the context can be put straight after. */
struct vfsmount *vfs_create_mount(struct fs_context *fc)
{
	struct vfsmount *mnt;

	if (!fc->root)
		return ERR_PTR(-EINVAL);
	mnt = kzalloc(sizeof(*mnt), GFP_KERNEL);
	if (!mnt)
		return ERR_PTR(-ENOMEM);
	mnt->mnt_root = dget(fc->root);
	mnt->mnt_sb = fc->root->d_sb;
	atomic_inc(&mnt->mnt_sb->s_active);
	return mnt;
}

void kern_unmount(struct vfsmount *mnt)
{
	if (!mnt)
		return;
	if (mnt->mnt_root) {
		struct super_block *sb = mnt->mnt_sb;

		dput(mnt->mnt_root);
		if (sb)
			deactivate_super(sb);
	}
	kfree(mnt);
}

/*
 * Pick a subtree out of a mounted filesystem, and drop the caller's reference
 * to the mount.
 *
 * btrfs uses it to mount a subvolume: the device's real root is mounted first,
 * the named subvolume is looked up inside it, and that dentry becomes the root
 * of the mount userspace sees. The reference dance is upstream's — the caller
 * hands over its vfsmount reference either way, which is why a failure here
 * still releases it.
 */
struct dentry *mount_subtree(struct vfsmount *mnt, const char *path)
{
	struct dentry *root;

	if (IS_ERR(mnt))
		return ERR_CAST(mnt);
	if (!mnt || !mnt->mnt_root)
		return ERR_PTR(-EINVAL);

	root = mnt->mnt_root;

	/*
	 * An empty path, or "/", means the filesystem's own root — which is what
	 * a plain `mount /dev/sda /mnt` asks for, and the only case reachable
	 * until a path walk exists here. A named subvolume needs the lookup that
	 * b1nix's VFS performs above this layer, so it is refused rather than
	 * silently returning the wrong root.
	 */
	if (path && path[0] && strcmp(path, "/") != 0) {
		/* Name the path: "the mount failed" alone does not say that what was
		 * asked for was a subvolume rather than the filesystem's root. */
		lkpi_printk("lkpi-fs: mount_subtree cannot walk to '%s'\n", path);
		kern_unmount(mnt);
		return ERR_PTR(-EOPNOTSUPP);
	}

	/* The dentry outlives the vfsmount, so it takes its own reference before
	 * the mount is dropped. */
	dget(root);
	kfree(mnt);
	return root;
}

/* ── the holder ─────────────────────────────────────────────────── */

/*
 * What the block layer calls when the device under a mounted filesystem goes
 * away or needs syncing. `mark_dead` is the interesting one: it is how a
 * filesystem learns its device has been removed, and a NULL there means it
 * carries on issuing I/O to a device that is gone.
 */
static void lkpi_fs_bdev_mark_dead(struct block_device *bdev)
{
	(void)bdev;
	/* b1nix's block devices do not disappear while mounted — there is no
	 * hotplug removal path that can take one out from under a filesystem —
	 * so there is nothing to report. This is where that would arrive. */
}

static void lkpi_fs_bdev_sync(struct block_device *bdev)
{
	sync_blockdev(bdev);
}

const struct blk_holder_ops fs_holder_ops = {
	.mark_dead = lkpi_fs_bdev_mark_dead,
	.sync = lkpi_fs_bdev_sync,
};

/* The /sys/fs directory every filesystem hangs its own under. */
struct kobject *fs_kobj;
