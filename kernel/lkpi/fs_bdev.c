/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the block device, bridged to b1nix's.
 *
 * A filesystem asks for a device by path, reads its geometry, and submits I/O
 * to it. This file is where the Linux `struct block_device` a filesystem holds
 * is paired with the b1nix one that actually has the disk behind it, and where
 * every geometry question is answered from that pairing.
 *
 * The pairing is one-to-one and permanent for the life of the boot: a b1nix
 * device gets a Linux object the first time somebody asks for it, and keeps it.
 * That matters because a filesystem compares `bd_dev` and the pointer itself to
 * decide whether two mounts are of the same device — two objects for one disk
 * would let a filesystem mount itself twice and corrupt both.
 *
 * Every number reported here comes from the b1nix device. Nothing is guessed:
 * a wrong logical block size makes a filesystem address the disk in the wrong
 * units, and it fails as corruption rather than as an error.
 */

#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/bio.h>
#include <linux/writeback.h>
#include <lkpi/env.h>

/*
 * b1nix's block layer, declared here rather than included: <b1nix/blk.h>
 * defines its own `struct block_device`, and this translation unit already has
 * the Linux one. The names are the same and the types are not, which is exactly
 * the collision <lkpi/env.h> describes.
 */
struct b1nix_block_device;
struct b1nix_block_device *lkpi_blk_get(const char *name);
struct b1nix_block_device *lkpi_blk_from_devno(u64 rdev);
u64 lkpi_blk_block_count(struct b1nix_block_device *dev);
unsigned int lkpi_blk_block_size(struct b1nix_block_device *dev);
u32 lkpi_blk_devno(struct b1nix_block_device *dev);
const char *lkpi_blk_name(struct b1nix_block_device *dev);
int lkpi_blk_is_rotational(struct b1nix_block_device *dev);
int lkpi_blk_discard_supported(struct b1nix_block_device *dev);
int lkpi_blk_discard(struct b1nix_block_device *dev, u64 lba, u32 count);
int lkpi_blk_zero(struct b1nix_block_device *dev, u64 lba, u32 count);
void lkpi_blk_flush(struct b1nix_block_device *dev);
void lkpi_blk_invalidate(struct b1nix_block_device *dev);
u32 lkpi_blk_max_sectors(struct b1nix_block_device *dev);

/* ── the device's own page cache ────────────────────────────────── */

/*
 * A block device has an inode, and that inode's mapping IS how a filesystem
 * reads the device before it has a filesystem.
 *
 * btrfs finds its superblock with
 * `read_cache_page_gfp(bdev->bd_inode->i_mapping, ...)` — so without this the
 * mount cannot even begin. The mapping's read_folio submits an ordinary read of
 * the corresponding sectors, which is what makes a page of the device appear
 * like a page of a file.
 *
 * The same cache is what makes the metadata a filesystem writes visible to
 * anything that reads the device afterwards: one cache, not two.
 */
static int bdev_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping ? folio->mapping->host : NULL;
	struct block_device *bdev = inode ? inode->i_private : NULL;
	struct bio *bio;
	int ret;

	(void)file;
	if (!bdev) {
		folio_unlock(folio);
		return -EIO;
	}

	bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_KERNEL);
	if (!bio) {
		folio_unlock(folio);
		return -ENOMEM;
	}
	/* The folio's index is in pages and a bio's sector is in 512-byte units,
	 * so the shift is PAGE_SHIFT - SECTOR_SHIFT. Getting it wrong reads from
	 * eight times too far into the disk, which looks like corruption. */
	bio->bi_iter.bi_sector = (sector_t)folio->index << (PAGE_SHIFT - SECTOR_SHIFT);
	bio_add_folio_nofail(bio, folio, folio_size(folio), 0);

	ret = submit_bio_wait(bio);
	bio_put(bio);

	if (ret == 0)
		folio_mark_uptodate(folio);
	/* read_folio unlocks the folio whether it succeeded or not: that is its
	 * contract, and a caller that unlocked again would release a lock somebody
	 * else has since taken. */
	folio_unlock(folio);
	return ret;
}

static int bdev_writepages(struct address_space *mapping,
                           struct writeback_control *wbc)
{
	extern int lkpi_write_dirty_buffers(struct address_space *mapping);

	(void)wbc;
	/* A filesystem marks buffers of this mapping dirty and expects a writeback
	 * of it to write them: jbd2's recovery does exactly that. Each dirty buffer
	 * goes out through its own bio. */
	return lkpi_write_dirty_buffers(mapping) < 0 ? -EIO : 0;
}

static const struct address_space_operations bdev_aops = {
	.read_folio = bdev_read_folio,
	.writepages = bdev_writepages,
};

/*
 * The inode behind a device.
 *
 * Allocated once with the link, and never freed for the same reason the link is
 * not: it is the identity of the device's cache, and a second one would give a
 * filesystem a second view of the same disk.
 */
static struct inode *bdev_make_inode(struct block_device *bdev)
{
	struct inode *inode = kzalloc(sizeof(*inode), GFP_KERNEL);

	if (!inode)
		return NULL;
	inode->i_mapping = &inode->i_data;
	inode->i_data.host = inode;
	inode->i_data.a_ops = &bdev_aops;
	inode->i_data.gfp_mask = GFP_KERNEL;
	xa_init(&inode->i_data.i_pages);
	init_rwsem(&inode->i_data.invalidate_lock);
	spin_lock_init(&inode->i_data.i_private_lock);
	INIT_LIST_HEAD(&inode->i_data.i_private_list);
	spin_lock_init(&inode->i_lock);
	init_rwsem(&inode->i_rwsem);
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_io_list);
	INIT_LIST_HEAD(&inode->i_lru);
	INIT_LIST_HEAD(&inode->i_sb_list);
	INIT_LIST_HEAD(&inode->i_wb_list);
	INIT_LIST_HEAD(&inode->i_devices);
	atomic_set(&inode->i_count, 1);
	inode->i_mode = S_IFBLK | 0600;
	inode->i_blkbits = PAGE_SHIFT;
	/* How the mapping gets back to the device it describes. */
	inode->i_private = bdev;
	inode->i_size = (loff_t)bdev->bd_nr_sectors << SECTOR_SHIFT;
	return inode;
}

/* ── the pairing ────────────────────────────────────────────────── */

struct lkpi_bdev_link {
	struct block_device bdev;
	struct gendisk disk;
	struct request_queue queue;
	/*
	 * The device's kobject, and the name it carries.
	 *
	 * It is what bdev_kobj() returns, and imported code does two things with
	 * it: it names the device (`disk_kobj->name`) and it asks sysfs to link
	 * to it. A NULL there is not a neutral "no sysfs": btrfs dereferences it
	 * for the name before it ever reaches sysfs, and the mount failed at
	 * "creating sysfs device link for devid 1 failed: -22".
	 */
	struct kobject kobj;
	char name[32];
	struct lkpi_bdev_link *next;
};

static struct lkpi_bdev_link *bdev_links;
static spinlock_t bdev_link_lock;
static int bdev_link_ready;

static void bdev_link_lock_init(void)
{
	if (!bdev_link_ready) {
		spin_lock_init(&bdev_link_lock);
		bdev_link_ready = 1;
	}
}

/*
 * Fill in the limits from the device.
 *
 * `logical_block_size` is the one that must be right: a filesystem divides its
 * own block size by it and addresses the disk in the result. `max_sectors` is
 * advisory here — b1nix's block cache chops a transfer at the device's ceiling
 * itself — but it is reported honestly so a filesystem sizing its I/O does not
 * build requests that will be split anyway.
 */
static void bdev_fill_limits(struct lkpi_bdev_link *link,
                             struct b1nix_block_device *dev)
{
	unsigned int bs = lkpi_blk_block_size(dev);

	if (!bs)
		bs = 512;
	link->queue.limits.logical_block_size = bs;
	link->queue.limits.physical_block_size = bs;
	link->queue.limits.io_min = bs;
	link->queue.limits.io_opt = bs;
	link->queue.limits.max_sectors = lkpi_blk_max_sectors(dev);
	link->queue.limits.max_hw_sectors = link->queue.limits.max_sectors;
	link->queue.limits.max_segments = 128;
	link->queue.limits.max_segment_size = 65536;
	link->queue.limits.dma_alignment = 511;
	link->queue.limits.zoned = BLK_ZONED_NONE;

	if (lkpi_blk_discard_supported(dev)) {
		link->queue.limits.max_discard_sectors = link->queue.limits.max_sectors;
		link->queue.limits.max_hw_discard_sectors =
			link->queue.limits.max_sectors;
		link->queue.limits.discard_granularity = bs;
	}
	/* Every b1nix block device is written through a cache that must be
	 * flushed, so the write-cache flag is set for all of them — a filesystem
	 * that believed otherwise would skip the flush before a barrier. */
	link->queue.queue_flags = 1UL << QUEUE_FLAG_WC;
	if (!lkpi_blk_is_rotational(dev))
		link->queue.queue_flags |= 1UL << QUEUE_FLAG_NONROT;
}

static struct block_device *bdev_for(struct b1nix_block_device *dev)
{
	struct lkpi_bdev_link *link;
	unsigned long flags;

	if (!dev)
		return NULL;

	bdev_link_lock_init();
	spin_lock_irqsave(&bdev_link_lock, flags);
	for (link = bdev_links; link; link = link->next)
		if (link->bdev.bd_b1nix == (void *)dev) {
			spin_unlock_irqrestore(&bdev_link_lock, flags);
			return &link->bdev;
		}
	spin_unlock_irqrestore(&bdev_link_lock, flags);

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		return NULL;

	link->bdev.bd_b1nix = (void *)dev;
	link->bdev.bd_dev = lkpi_blk_devno(dev);
	link->bdev.bd_disk = &link->disk;
	{
		const char *n = lkpi_blk_name(dev);
		usize len = n ? strlen(n) : 0;

		if (len > sizeof(link->name) - 1)
			len = sizeof(link->name) - 1;
		if (len)
			memcpy(link->name, n, len);
		link->name[len] = '\0';
		memcpy(link->disk.disk_name, link->name, len + 1);
		link->kobj.name = link->name;
	}
	link->bdev.bd_block_size = lkpi_blk_block_size(dev);
	link->disk.queue = &link->queue;
	bdev_fill_limits(link, dev);
	/*
	 * The size in 512-byte sectors, whatever the device's own block size —
	 * `bdev_nr_sectors` is defined in those units and a filesystem multiplies
	 * it by 512 to get the byte size.
	 */
	link->bdev.bd_nr_sectors = lkpi_blk_block_count(dev) *
	                           (link->bdev.bd_block_size / 512);
	INIT_LIST_HEAD(&link->bdev.bd_list);
	atomic_set(&link->bdev.bd_openers, 0);
	/* The device's own page cache, which is how a filesystem reads it before
	 * it has a filesystem. */
	link->bdev.bd_inode = bdev_make_inode(&link->bdev);
	if (!link->bdev.bd_inode) {
		kfree(link);
		return NULL;
	}
	link->bdev.bd_mapping = link->bdev.bd_inode->i_mapping;

	spin_lock_irqsave(&bdev_link_lock, flags);
	/* Re-check: another caller may have created one while we allocated. Theirs
	 * wins, because something may already be holding it. */
	{
		struct lkpi_bdev_link *existing;

		for (existing = bdev_links; existing; existing = existing->next)
			if (existing->bdev.bd_b1nix == (void *)dev) {
				spin_unlock_irqrestore(&bdev_link_lock, flags);
				kfree(link);
				return &existing->bdev;
			}
	}
	link->next = bdev_links;
	bdev_links = link;
	spin_unlock_irqrestore(&bdev_link_lock, flags);
	return &link->bdev;
}

/* ── opening ────────────────────────────────────────────────────── */

/*
 * Resolve the device name a mount option gave.
 *
 * b1nix names its disks `sata0`, `nvme0n1` and so on, and a mount passes
 * `/dev/sata0`. The leading directory is stripped here rather than in the
 * filesystem, which never sees a b1nix path.
 */
static const char *bdev_name_of(const char *path)
{
	const char *slash;

	if (!path)
		return NULL;
	slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

struct block_device *blkdev_get_by_path(const char *path, blk_mode_t mode,
                                        void *holder,
                                        const struct blk_holder_ops *hops)
{
	struct b1nix_block_device *dev;
	struct block_device *bdev;

	(void)hops;
	dev = lkpi_blk_get(bdev_name_of(path));
	if (!dev)
		return ERR_PTR(-ENOENT);

	bdev = bdev_for(dev);
	if (!bdev)
		return ERR_PTR(-ENOMEM);

	/*
	 * BLK_OPEN_EXCL with a holder is what stops the same device being mounted
	 * twice. The holder is remembered rather than counted: a second exclusive
	 * open by a DIFFERENT holder is refused, and by the same holder is the
	 * filesystem re-opening its own device, which btrfs does per subvolume.
	 */
	if (mode & BLK_OPEN_EXCL) {
		if (bdev->bd_holder && bdev->bd_holder != holder)
			return ERR_PTR(-EBUSY);
		bdev->bd_holder = holder;
	}
	bdev->bd_read_only = (mode & BLK_OPEN_WRITE) ? 0 : 1;
	atomic_inc(&bdev->bd_openers);
	return bdev;
}

struct block_device *blkdev_get_by_dev(dev_t dev, blk_mode_t mode, void *holder,
                                       const struct blk_holder_ops *hops)
{
	struct b1nix_block_device *b1dev = lkpi_blk_from_devno(dev);
	struct block_device *bdev;

	(void)hops;
	if (!b1dev)
		return ERR_PTR(-ENOENT);
	bdev = bdev_for(b1dev);
	if (!bdev)
		return ERR_PTR(-ENOMEM);
	if (mode & BLK_OPEN_EXCL) {
		if (bdev->bd_holder && bdev->bd_holder != holder)
			return ERR_PTR(-EBUSY);
		bdev->bd_holder = holder;
	}
	atomic_inc(&bdev->bd_openers);
	return bdev;
}

void blkdev_put(struct block_device *bdev, void *holder)
{
	if (!bdev)
		return;
	if (bdev->bd_holder == holder)
		bdev->bd_holder = NULL;
	atomic_dec(&bdev->bd_openers);
	/*
	 * The link itself is kept. It is the identity of the device — a
	 * filesystem that mounts, unmounts and mounts again must get the same
	 * object, and freeing it here would give it a different one.
	 */
}

dev_t lookup_bdev(const char *pathname, dev_t *dev)
{
	struct b1nix_block_device *b1dev = lkpi_blk_get(bdev_name_of(pathname));

	if (!b1dev)
		return -ENOENT;
	if (dev)
		*dev = lkpi_blk_devno(b1dev);
	return 0;
}

/* ── geometry ───────────────────────────────────────────────────── */

sector_t bdev_nr_sectors(struct block_device *bdev)
{
	return bdev ? bdev->bd_nr_sectors : 0;
}

struct request_queue *bdev_get_queue(struct block_device *bdev)
{
	return bdev && bdev->bd_disk ? bdev->bd_disk->queue : NULL;
}

int bdev_set_blocksize(struct block_device *bdev, int size)
{
	/*
	 * The size a filesystem will address the device in. It may be larger than
	 * the device's own block size — a 4 KiB filesystem on a 512-byte disk —
	 * but never smaller, because the device cannot address a fraction of its
	 * own block.
	 */
	if (size < 512 || (size & (size - 1)))
		return -EINVAL;
	if (bdev && size < (int)bdev_logical_block_size(bdev))
		return -EINVAL;
	if (bdev && bdev->bd_block_size != (unsigned int)size) {
		/*
		 * Drop the device's cached folios, as upstream's kill_bdev does.
		 * They are divided into buffers of the old size, and a buffer lookup
		 * at the new size refuses such a folio: ext4 reads its superblock at
		 * 1 KiB, switches to the filesystem's 4 KiB and read it again, and
		 * the second read failed ("Can't read superblock on 2nd try").
		 */
		sync_blockdev(bdev);
		if (bdev->bd_inode)
			invalidate_mapping_pages(bdev->bd_inode->i_mapping, 0, (pgoff_t)-1);
	}
	if (bdev)
		bdev->bd_block_size = (unsigned int)size;
	return 0;
}

/* 6.10+: the device is named by the file it was opened as. */
int set_blocksize(struct file *file, int size)
{
	return bdev_set_blocksize(file_bdev(file), size);
}

struct device *disk_to_dev(struct gendisk *disk)
{
	return disk ? disk->part0_dev : NULL;
}

struct kobject *bdev_kobj(struct block_device *bdev)
{
	/*
	 * The device's kobject. It carries the device's name and no sysfs
	 * directory: b1nix publishes block devices under /sys from its own
	 * registry, so sysfs_create_link() against it finds nothing to point at
	 * and succeeds without making a dangling link.
	 */
	if (!bdev)
		return NULL;
	return &((struct lkpi_bdev_link *)bdev)->kobj;
}

/* The name b1nix knows the device by, for "%pg". */
const char *lkpi_bdev_printk_name(const void *bdev)
{
	const struct lkpi_bdev_link *link = bdev;

	return (link && link->name[0]) ? link->name : NULL;
}

bool bdev_iter_is_aligned(struct block_device *bdev, struct iov_iter *iter)
{
	unsigned int bs = bdev ? bdev_logical_block_size(bdev) : 512;

	return iov_iter_is_aligned(iter, bs - 1, bs - 1);
}

/* ── whole-device operations ────────────────────────────────────── */

int sync_blockdev(struct block_device *bdev)
{
	extern int lkpi_write_dirty_buffers(struct address_space *mapping);

	if (!bdev || !bdev->bd_b1nix)
		return 0;
	/*
	 * The device's dirty buffers first, as upstream's filemap_write_and_wait
	 * on bd_mapping does, then b1nix's block cache to the medium.
	 *
	 * This used to flush the block cache only. jbd2's recovery replays every
	 * committed transaction into the device's buffers, marks them dirty, calls
	 * this -- and then marks the journal empty, with FUA. The replayed blocks
	 * were never written: after any unclean shutdown the transactions the
	 * journal existed to save were dropped, and ext4 found block bitmaps
	 * disagreeing with their group descriptors (on the phone, after every
	 * forced reset).
	 */
	if (bdev->bd_inode && bdev->bd_inode->i_mapping &&
	    lkpi_write_dirty_buffers(bdev->bd_inode->i_mapping) < 0)
		return -EIO;
	lkpi_blk_flush(bdev->bd_b1nix);
	return 0;
}

int sync_blockdev_range(struct block_device *bdev, loff_t lstart, loff_t lend)
{
	(void)lstart;
	(void)lend;
	/* b1nix's block cache flushes a whole device; a range flush would be a
	 * finer promise than it can keep, and flushing more than asked is always
	 * safe. */
	return sync_blockdev(bdev);
}

void invalidate_bdev(struct block_device *bdev)
{
	if (!bdev)
		return;
	/*
	 * The device's own page cache goes too, not only b1nix's block cache.
	 * Those folios carry the buffer heads a filesystem reads its metadata
	 * through, and they are sized for the block size that filesystem chose.
	 * Left behind after an unmount, the next mount finds a folio whose
	 * buffers are the wrong size, gets NULL from __getblk, and reports
	 * ENOMEM -- an unmount followed by a mount of the same device failed
	 * every time, with hundreds of megabytes free.
	 */
	if (bdev->bd_inode && bdev->bd_inode->i_mapping)
		invalidate_mapping_pages(bdev->bd_inode->i_mapping, 0, (pgoff_t)-1);
	if (bdev->bd_b1nix)
		lkpi_blk_invalidate(bdev->bd_b1nix);
}

int blkdev_issue_flush(struct block_device *bdev)
{
	return sync_blockdev(bdev);
}

static u64 bdev_sector_to_lba(struct block_device *bdev, sector_t sector)
{
	unsigned int bs = bdev_logical_block_size(bdev);

	return ((u64)sector << SECTOR_SHIFT) / (bs ? bs : 512);
}

static u32 bdev_sectors_to_blocks(struct block_device *bdev, sector_t nr)
{
	unsigned int bs = bdev_logical_block_size(bdev);

	return (u32)(((u64)nr << SECTOR_SHIFT) / (bs ? bs : 512));
}

int blkdev_issue_discard(struct block_device *bdev, sector_t sector,
                         sector_t nr_sects, gfp_t gfp_mask)
{
	(void)gfp_mask;
	if (!bdev || !bdev->bd_b1nix)
		return -ENODEV;
	if (!lkpi_blk_discard_supported(bdev->bd_b1nix))
		/* Not an error: a discard is advice, and a device that cannot take it
		 * has lost nothing. Reporting a failure would make a filesystem log
		 * one per trim. */
		return 0;
	return lkpi_blk_discard(bdev->bd_b1nix, bdev_sector_to_lba(bdev, sector),
	                        bdev_sectors_to_blocks(bdev, nr_sects));
}

int __blkdev_issue_discard(struct block_device *bdev, sector_t sector,
                           sector_t nr_sects, gfp_t gfp, struct bio **biop)
{
	/* The asynchronous form builds a bio chain for the caller to submit.
	 * b1nix's discard is synchronous, so it is issued here and no bio is
	 * produced — which the caller handles, because a NULL chain is what "no
	 * more to submit" means. */
	(void)biop;
	return blkdev_issue_discard(bdev, sector, nr_sects, gfp);
}

int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
                         sector_t nr_sects, gfp_t gfp_mask, unsigned flags)
{
	(void)gfp_mask;
	(void)flags;
	if (!bdev || !bdev->bd_b1nix)
		return -ENODEV;
	return lkpi_blk_zero(bdev->bd_b1nix, bdev_sector_to_lba(bdev, sector),
	                     bdev_sectors_to_blocks(bdev, nr_sects));
}

int sb_issue_discard(struct super_block *sb, sector_t block, sector_t nr_blocks,
                     gfp_t gfp_mask, unsigned long flags)
{
	sector_t shift = sb->s_blocksize_bits - SECTOR_SHIFT;

	(void)flags;
	return blkdev_issue_discard(sb->s_bdev, block << shift,
	                            nr_blocks << shift, gfp_mask);
}

int sb_issue_zeroout(struct super_block *sb, sector_t block, sector_t nr_blocks,
                     gfp_t gfp_mask)
{
	sector_t shift = sb->s_blocksize_bits - SECTOR_SHIFT;

	return blkdev_issue_zeroout(sb->s_bdev, block << shift,
	                            nr_blocks << shift, gfp_mask, 0);
}

int freeze_bdev(struct block_device *bdev)
{
	return sync_blockdev(bdev);
}

int thaw_bdev(struct block_device *bdev)
{
	(void)bdev;
	return 0;
}

int bdev_freeze(struct block_device *bdev)
{
	return sync_blockdev(bdev);
}

int bdev_thaw(struct block_device *bdev)
{
	(void)bdev;
	return 0;
}

/* ── zoned devices ──────────────────────────────────────────────── */

/*
 * No b1nix device is zoned, and `bdev_is_zoned` says so — which is what keeps
 * btrfs's zoned paths unreached. These exist because the calls are compiled
 * either way, and each refuses rather than succeeding: a silent success on a
 * zone reset would tell btrfs a region had been rewound when it had not.
 */
int blkdev_report_zones(struct block_device *bdev, sector_t sector,
                        unsigned int nr_zones, report_zones_cb cb, void *data)
{
	(void)bdev; (void)sector; (void)nr_zones; (void)cb; (void)data;
	return -EOPNOTSUPP;
}

int blkdev_zone_mgmt(struct block_device *bdev, enum req_op op, sector_t sector,
                     sector_t nr_sectors, gfp_t gfp_mask)
{
	(void)bdev; (void)op; (void)sector; (void)nr_sectors; (void)gfp_mask;
	return -EOPNOTSUPP;
}

/* ── limit stacking ─────────────────────────────────────────────── */

void blk_set_stacking_limits(struct queue_limits *lim)
{
	memset(lim, 0, sizeof(*lim));
	lim->logical_block_size = 512;
	lim->physical_block_size = 512;
	lim->io_min = 512;
	lim->max_sectors = 2560;
	lim->max_hw_sectors = 2560;
	lim->max_segments = 128;
	lim->max_segment_size = 65536;
	lim->dma_alignment = 511;
}

int blk_stack_limits(struct queue_limits *t, struct queue_limits *b,
                     sector_t start)
{
	(void)start;
	/*
	 * The stacked limits are the MOST RESTRICTIVE of the two: a device built
	 * over several must satisfy all of them. Taking the maximum instead
	 * builds requests one of the underlying devices cannot accept.
	 */
	if (b->logical_block_size > t->logical_block_size)
		t->logical_block_size = b->logical_block_size;
	if (b->physical_block_size > t->physical_block_size)
		t->physical_block_size = b->physical_block_size;
	if (b->io_min > t->io_min)
		t->io_min = b->io_min;
	if (!t->max_sectors || (b->max_sectors && b->max_sectors < t->max_sectors))
		t->max_sectors = b->max_sectors;
	if (!t->max_segments || (b->max_segments &&
	                         b->max_segments < t->max_segments))
		t->max_segments = b->max_segments;
	if (!t->max_segment_size ||
	    (b->max_segment_size && b->max_segment_size < t->max_segment_size))
		t->max_segment_size = b->max_segment_size;
	if (b->dma_alignment > t->dma_alignment)
		t->dma_alignment = b->dma_alignment;
	return 0;
}

void blk_update_request(struct request *rq, blk_status_t error,
                        unsigned int nr_bytes)
{
	(void)rq; (void)error; (void)nr_bytes;
	/* There are no requests: b1nix has no I/O scheduler, and a bio goes
	 * straight into the block cache. See <linux/blkdev.h>. */
}

struct cgroup_subsys_state * const blkcg_root_css;

/* ── block devices as files ─────────────────────────────────────── */

/*
 * The file is a handle on an open of the device, not something installed in a
 * descriptor table: the holder rides in private_data so the close releases the
 * same exclusive claim the open took.
 */
static struct file *bdev_file_for(struct block_device *bdev, blk_mode_t mode,
                                  void *holder)
{
	struct file *file;

	if (IS_ERR(bdev))
		return ERR_CAST(bdev);
	file = kzalloc(sizeof(*file), GFP_KERNEL);
	if (!file) {
		blkdev_put(bdev, holder);
		return ERR_PTR(-ENOMEM);
	}
	file->f_inode = bdev->bd_inode;
	file->f_mapping = bdev->bd_mapping;
	file->f_mode = FMODE_READ | ((mode & BLK_OPEN_WRITE) ? FMODE_WRITE : 0);
	file->private_data = holder;
	atomic_long_set(&file->f_count, 1);
	return file;
}

struct file *bdev_file_open_by_path(const char *path, blk_mode_t mode,
                                    void *holder, const struct blk_holder_ops *hops)
{
	return bdev_file_for(blkdev_get_by_path(path, mode, holder, hops), mode,
	                     holder);
}

struct file *bdev_file_open_by_dev(dev_t dev, blk_mode_t mode, void *holder,
                                   const struct blk_holder_ops *hops)
{
	return bdev_file_for(blkdev_get_by_dev(dev, mode, holder, hops), mode,
	                     holder);
}

struct block_device *file_bdev(struct file *bdev_file)
{
	return bdev_file->f_inode->i_private;
}

void bdev_fput(struct file *bdev_file)
{
	if (!bdev_file)
		return;
	if (atomic64_sub_return(1, &bdev_file->f_count) != 0)
		return;
	blkdev_put(file_bdev(bdev_file), bdev_file->private_data);
	kfree(bdev_file);
}

int bio_split_rw_at(struct bio *bio, const struct queue_limits *lim,
                    unsigned *segs, unsigned max_bytes)
{
	(void)lim;
	if (segs)
		*segs = bio->bi_vcnt;
	if (bio->bi_iter.bi_size <= max_bytes)
		return 0;
	return (int)(max_bytes >> SECTOR_SHIFT);
}

/*
 * Read or write `len` bytes of a kernel buffer at `sector`, waiting for it.
 * The buffer need not be page-aligned or physically contiguous: each page it
 * spans is added as its own segment.
 */
int bdev_rw_virt(struct block_device *bdev, sector_t sector, void *data,
                 size_t len, enum req_op op)
{
	unsigned int nr = (unsigned int)(DIV_ROUND_UP(offset_in_page(data) + len,
	                                              PAGE_SIZE));
	struct bio *bio = bio_alloc(bdev, nr, op, GFP_KERNEL);
	size_t done = 0;
	int ret;

	if (!bio)
		return -ENOMEM;
	bio->bi_iter.bi_sector = sector;
	while (done < len) {
		void *p = (char *)data + done;
		unsigned int off = (unsigned int)offset_in_page(p);
		unsigned int chunk = (unsigned int)min_t(size_t, PAGE_SIZE - off,
		                                         len - done);

		if (bio_add_page(bio, virt_to_page(p), chunk, off) != (int)chunk) {
			bio_put(bio);
			return -EIO;
		}
		done += chunk;
	}
	ret = submit_bio_wait(bio);
	bio_put(bio);
	return ret;
}
