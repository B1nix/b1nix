/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BLKDEV_H
#define LKPI_LINUX_BLKDEV_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/blk_types.h>
/* The bio API. Upstream's blkdev.h includes it too, and the direction matters:
 * fs/iomap reaches bio_alloc through <linux/iomap.h> -> <linux/blkdev.h> and
 * includes neither itself. */
#include <linux/bio.h>
#include <linux/backing-dev.h>
/* struct blk_zone. Declared complete because btrfs puts arrays of it on the
 * stack in code compiled whether or not any device is zoned. */
#include <linux/blkzoned.h>

/*
 * The block device, as a filesystem sees it.
 *
 * This is the narrowest of the block-layer headers to get right, because a
 * filesystem asks it questions whose wrong answer is silent corruption rather
 * than a failure: how large is the device, what is its logical block size, is
 * it read-only, does it support discard, is it zoned. Every one of those is
 * answered here from b1nix's own block layer through lkpi_bdev_*, and none of
 * them is allowed to be a guess.
 *
 * What is deliberately NOT modelled: the request queue as a queue. b1nix has
 * one block cache with its own writeback, and `struct request_queue` here
 * carries limits and nothing else — no elevator, no tags, no requests. Imported
 * code that reaches for a request would fail to compile, which is the correct
 * outcome: it would mean a path that needs a scheduler this kernel has not got.
 */

struct inode;
struct gendisk;
struct block_device;

/* The longest device name the block layer will render, including the NUL. It is
 * a buffer size callers declare arrays with, so it is ABI between them. */
#define BDEVNAME_SIZE 32
#define DISK_NAME_LEN 32

/* SECTOR_SHIFT and SECTOR_SIZE come from <linux/blk_types.h>. */
#define PAGE_SECTORS_SHIFT (PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS       (1 << PAGE_SECTORS_SHIFT)

/* Sectors are always 512 bytes at this interface regardless of what the device
 * reports as its logical block size. Upstream is the same, and mixing the two
 * up is the classic way to write a filesystem at four times its intended
 * offset. */
#define SECTOR_MASK (PAGE_SECTORS - 1)

struct queue_limits {
	unsigned int logical_block_size;
	unsigned int physical_block_size;
	unsigned int io_min;
	unsigned int io_opt;
	unsigned int max_sectors;
	unsigned int max_hw_sectors;
	unsigned int max_segments;
	unsigned int max_segment_size;
	unsigned int max_discard_sectors;
	unsigned int max_hw_discard_sectors;
	unsigned int discard_granularity;
	unsigned int max_write_zeroes_sectors;
	unsigned int max_zone_append_sectors;
	unsigned int max_active_zones;
	unsigned int max_open_zones;
	unsigned int dma_alignment;
	unsigned int chunk_sectors;   /* zone size, when zoned */
	enum blk_zoned_model zoned;
	unsigned char misaligned;
	unsigned short alignment_offset;
};

struct request_queue {
	struct queue_limits limits;
	struct backing_dev_info *backing_dev_info;
	unsigned long queue_flags;
};

enum {
	QUEUE_FLAG_NONROT = 1,
	QUEUE_FLAG_STABLE_WRITES,
	QUEUE_FLAG_WC,          /* has a volatile write cache */
	QUEUE_FLAG_FUA,
	QUEUE_FLAG_DISCARD,
	QUEUE_FLAG_SAME_COMP,
	QUEUE_FLAG_DYING,
};

struct gendisk {
	struct request_queue *queue;
	/* The driver model device behind the disk. btrfs reaches it to name the
	 * device in sysfs and to hang its own symlink off it. */
	struct device *part0_dev;
	struct backing_dev_info *bdi;
	unsigned int flags;
	char disk_name[32];
	void *private_data;
};

/*
 * `bd_inode` is the block device's own page-cache inode — what buffer_head
 * caching hangs off, and what `sb_bread` reads through. It is a real inode here
 * for the same reason it is one upstream: a filesystem's metadata cache and the
 * device's page cache have to be the same cache, or a buffer written by one is
 * invisible to the other.
 */
struct block_device {
	dev_t bd_dev;
	struct inode *bd_inode;
	struct gendisk *bd_disk;
	struct super_block *bd_super;
	void *bd_holder;
	int bd_read_only;
	unsigned int bd_block_size;
	sector_t bd_nr_sectors;
	/* b1nix's own device behind this one, opaque on this side of the
	 * boundary; the lkpi block code casts it back. See <lkpi/env.h>. */
	void *bd_b1nix;
	atomic_t bd_openers;
	struct list_head bd_list;
};

/* ── how a device is opened ─────────────────────────────────────── */

typedef unsigned int blk_mode_t;

#define BLK_OPEN_READ       ((blk_mode_t)(1 << 0))
#define BLK_OPEN_WRITE      ((blk_mode_t)(1 << 1))
#define BLK_OPEN_EXCL       ((blk_mode_t)(1 << 2))
#define BLK_OPEN_NDELAY     ((blk_mode_t)(1 << 3))
#define BLK_OPEN_WRITE_IOCTL ((blk_mode_t)(1 << 4))
#define BLK_OPEN_RESTRICT_WRITES ((blk_mode_t)(1 << 5))

struct blk_holder_ops {
	void (*mark_dead)(struct block_device *bdev);
	void (*sync)(struct block_device *bdev);
};

struct block_device *blkdev_get_by_path(const char *path, blk_mode_t mode,
                                        void *holder,
                                        const struct blk_holder_ops *hops);
struct block_device *blkdev_get_by_dev(dev_t dev, blk_mode_t mode, void *holder,
                                       const struct blk_holder_ops *hops);
void blkdev_put(struct block_device *bdev, void *holder);
int bdev_freeze(struct block_device *bdev);
int bdev_thaw(struct block_device *bdev);
/* Both return an errno: 6.6 moved the superblock out of the interface, and a
 * caller assigning the result to an int is checking for failure. */
int freeze_bdev(struct block_device *bdev);
int thaw_bdev(struct block_device *bdev);

/* ── geometry and capability ────────────────────────────────────── */

/* The device behind a disk, for the sysfs paths that link to it. */
struct device *disk_to_dev(struct gendisk *disk);

sector_t bdev_nr_sectors(struct block_device *bdev);
struct request_queue *bdev_get_queue(struct block_device *bdev);

static inline loff_t bdev_nr_bytes(struct block_device *bdev)
{
	return (loff_t)bdev_nr_sectors(bdev) << SECTOR_SHIFT;
}

static inline unsigned int bdev_logical_block_size(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.logical_block_size;
}

static inline unsigned int bdev_physical_block_size(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.physical_block_size;
}

static inline unsigned int bdev_io_min(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.io_min;
}

static inline unsigned int bdev_io_opt(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.io_opt;
}

static inline unsigned int bdev_max_discard_sectors(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.max_discard_sectors;
}

static inline unsigned int bdev_discard_granularity(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.discard_granularity;
}

static inline unsigned int bdev_max_zone_append_sectors(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.max_zone_append_sectors;
}

static inline unsigned int bdev_dma_alignment(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.dma_alignment;
}

static inline bool bdev_read_only(struct block_device *bdev)
{
	return bdev->bd_read_only != 0;
}

static inline bool bdev_nonrot(struct block_device *bdev)
{
	return (bdev_get_queue(bdev)->queue_flags & (1UL << QUEUE_FLAG_NONROT)) != 0;
}

static inline bool bdev_write_cache(struct block_device *bdev)
{
	return (bdev_get_queue(bdev)->queue_flags & (1UL << QUEUE_FLAG_WC)) != 0;
}

static inline bool bdev_fua(struct block_device *bdev)
{
	return (bdev_get_queue(bdev)->queue_flags & (1UL << QUEUE_FLAG_FUA)) != 0;
}

static inline bool bdev_stable_writes(struct block_device *bdev)
{
	return (bdev_get_queue(bdev)->queue_flags &
	        (1UL << QUEUE_FLAG_STABLE_WRITES)) != 0;
}

/* ── zoned devices ──────────────────────────────────────────────── */

/*
 * b1nix has no zoned block device: no driver reports one and none can be
 * attached. So these answer "not zoned" rather than being absent, because
 * btrfs's zoned support is compiled in and asks unconditionally at mount — and
 * a mount that cannot ask is a mount that fails.
 *
 * The management calls return -EOPNOTSUPP rather than success. A silent success
 * on a zone reset would tell btrfs a region had been rewound when it had not.
 */
static inline enum blk_zoned_model bdev_zoned_model(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.zoned;
}

static inline bool bdev_is_zoned(struct block_device *bdev)
{
	return bdev_zoned_model(bdev) != BLK_ZONED_NONE;
}

static inline sector_t bdev_zone_sectors(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.chunk_sectors;
}

static inline unsigned int bdev_max_open_zones(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.max_open_zones;
}

static inline unsigned int bdev_max_active_zones(struct block_device *bdev)
{
	return bdev_get_queue(bdev)->limits.max_active_zones;
}

static inline unsigned int bdev_zone_no(struct block_device *bdev, sector_t sec)
{
	sector_t zs = bdev_zone_sectors(bdev);

	return zs ? (unsigned int)(sec / zs) : 0;
}

struct blk_zone;
typedef int (*report_zones_cb)(struct blk_zone *zone, unsigned int idx,
                               void *data);

int blkdev_report_zones(struct block_device *bdev, sector_t sector,
                        unsigned int nr_zones, report_zones_cb cb, void *data);
int blkdev_zone_mgmt(struct block_device *bdev, enum req_op op, sector_t sector,
                     sector_t nr_sectors, gfp_t gfp_mask);

/* ── issuing whole-range operations ─────────────────────────────── */

int blkdev_issue_flush(struct block_device *bdev);
int blkdev_issue_discard(struct block_device *bdev, sector_t sector,
                         sector_t nr_sects, gfp_t gfp_mask);
int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
                         sector_t nr_sects, gfp_t gfp_mask, unsigned flags);

/* The superblock-relative forms: the sector numbers are in the filesystem's own
 * blocks, converted here rather than at each of the dozens of call sites — the
 * conversion is where an off-by-a-block-size lands. */
int sb_issue_discard(struct super_block *sb, sector_t block, sector_t nr_blocks,
                     gfp_t gfp_mask, unsigned long flags);
int sb_issue_zeroout(struct super_block *sb, sector_t block, sector_t nr_blocks,
                     gfp_t gfp_mask);

#define BLKDEV_ZERO_NOUNMAP   (1 << 0)
#define BLKDEV_ZERO_NOFALLBACK (1 << 1)

/* ── plugging ───────────────────────────────────────────────────── */

/*
 * A plug batches submissions so the layer below can merge them. b1nix's block
 * cache merges on its own and has no per-task submission list, so a plug here
 * records nothing and both calls are structural.
 *
 * They are kept rather than defined away because the pairing is not always
 * lexical: btrfs starts a plug in one function and finishes it in another, and
 * a macro that expanded to nothing would leave the plug variable unused in one
 * translation unit and undeclared in the next.
 */
struct blk_plug {
	struct list_head list;
	unsigned short nr;
};

void blk_start_plug(struct blk_plug *plug);
void blk_finish_plug(struct blk_plug *plug);
void blk_flush_plug(struct blk_plug *plug, bool from_schedule);
struct blk_plug_cb;
typedef void (*blk_plug_cb_fn)(struct blk_plug_cb *, bool);
struct blk_plug_cb {
	struct list_head list;
	blk_plug_cb_fn callback;
	void *data;
};
struct blk_plug_cb *blk_check_plugged(blk_plug_cb_fn unplug, void *data,
                                      int size);

/* Park while an I/O this task submitted is outstanding. There is no I/O
 * scheduler to hand the CPU to, so it is a plain yield. */
void blk_io_schedule(void);
struct task_struct;
void blk_wake_io_task(struct task_struct *waiter);

/* ── errors ─────────────────────────────────────────────────────── */

int blk_status_to_errno(blk_status_t status);
blk_status_t errno_to_blk_status(int errno);

/* ── limit stacking ─────────────────────────────────────────────── */

void blk_set_stacking_limits(struct queue_limits *lim);
int blk_stack_limits(struct queue_limits *t, struct queue_limits *b,
                     sector_t start);

struct kobject;
struct kobject *bdev_kobj(struct block_device *bdev);

struct iov_iter;
bool bdev_iter_is_aligned(struct block_device *bdev, struct iov_iter *iter);

struct request;
void blk_update_request(struct request *rq, blk_status_t error,
                        unsigned int nr_bytes);

/* Split of a sector count across a chunk boundary, used by btrfs to keep an I/O
 * inside one stripe. Upstream's arithmetic, kept exactly: the boundary is
 * relative to sector zero of the device, not to the start of the I/O. */
static inline unsigned int blk_max_size_offset(struct request_queue *q,
                                               sector_t offset,
                                               unsigned int chunk_sectors)
{
	if (!chunk_sectors)
		chunk_sectors = q->limits.chunk_sectors;
	if (!chunk_sectors)
		return q->limits.max_sectors;
	return (unsigned int)(chunk_sectors - (offset & (chunk_sectors - 1)));
}

/* The asynchronous discard: builds the bio chain and hands the caller the last
 * one to submit, so several ranges can be issued before waiting on any. */
int __blkdev_issue_discard(struct block_device *bdev, sector_t sector,
                           sector_t nr_sects, gfp_t gfp, struct bio **biop);
int __blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
                           sector_t nr_sects, gfp_t gfp, struct bio **biop,
                           unsigned flags);

/* log2 of a block size. Returns the shift, so a caller storing it in an inode's
 * i_blkbits gets the value that field means. */
static inline unsigned int blksize_bits(unsigned int size)
{
	unsigned int bits = 8;

	do {
		bits++;
		size >>= 1;
	} while (size > 256);
	return bits;
}

#define BLK_MAX_SEGMENT_SIZE 65536

/* The shared bio set the filesystems allocate from. */
extern struct bio_set fs_bio_set;

/* Poll a bio-backed iocb for completion, for a filesystem that advertises
 * IOCB_HIPRI. b1nix's block layer has no polled completion, so it reports none
 * ready rather than claiming an I/O finished. */
struct io_comp_batch;
int iocb_bio_iopoll(struct kiocb *kiocb, struct io_comp_batch *iob,
                    unsigned int flags);

#endif
