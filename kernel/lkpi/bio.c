/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the bio, and the path from one into b1nix's block layer.
 *
 * A bio is one I/O request: a device, a starting sector, and a vector of
 * (page, offset, length) runs. This file builds them, splits and chains them,
 * and — in submit_bio — turns one into calls on b1nix's block cache.
 *
 * Three properties of the model are load-bearing, and getting any of them wrong
 * corrupts data rather than failing:
 *
 *   - `bi_iter` is CONSUMED as the I/O proceeds. A submitted bio no longer
 *     describes what it originally asked for, which is why code that needs the
 *     original saves a copy of the iterator first.
 *   - A sector is 512 bytes here regardless of the device's logical block size.
 *     b1nix's block layer counts in the device's own blocks, so every
 *     submission converts, and the conversion is the one place that must be
 *     right.
 *   - Completion is a callback. `bi_end_io` runs when the last byte is done,
 *     and a chained bio's parent completes only when its children have.
 *
 * On the b1nix side of the boundary: b1nix and lkpi headers only, never a
 * linux/ one. The Linux
 * structures are mirrored in <lkpi/fs_abi.h>, and kernel/lkpi/fs_abi_check.c —
 * compiled against the real linux headers — asserts that the two agree.
 */

#include <lkpi/env.h>
#include <lkpi/types.h>
#include <lkpi/page.h>
#include <b1nix/blk.h>
#include <b1nix/klog.h>
#include <b1nix/sched.h>
#include <string.h>

#include <lkpi/fs_abi.h>

void *lkpi_kmalloc(usize size, u32 flags);
void lkpi_kfree(void *ptr);

/*
 * A bio set.
 *
 * Upstream it is a reserve, so that a path which must make progress can always
 * allocate. b1nix's kmalloc panics rather than blocking on OOM, so there is
 * nothing to reserve against — but `front_pad` is not part of the reserve, it
 * is layout, and it is load-bearing. See bio_alloc_common.
 */
struct lkpi_bio_set {
	unsigned int front_pad;
	unsigned int back_pad;
	int initialised;
};

static unsigned int bioset_front_pad(struct lkpi_bio_set *bs)
{
	return bs ? bs->front_pad : 0u;
}

/* ── allocation ─────────────────────────────────────────────────── */

/*
 * The set's front pad.
 *
 * A bio set can ask for bytes BEFORE the bio, and the filesystem's own
 * structure lives there: btrfs registers
 * `front_pad = offsetof(struct btrfs_bio, bio)` and reaches its bio through
 * container_of, so `btrfs_bio(bio)` is the allocation's base and the bio is
 * `front_pad` bytes into it.
 *
 * Ignoring the pad is not a missing optimisation. It puts the bio at the base,
 * so every btrfs_bio() points BEFORE the allocation — and the first thing
 * btrfs does with it is memset it to zero, which writes over whatever the heap
 * had there. It read and completed one metadata block correctly and then the
 * machine died somewhere else entirely.
 */
static struct lkpi_bio *bio_alloc_common(struct lkpi_block_device *bdev,
                                         unsigned short nr_vecs,
                                         unsigned int opf,
                                         struct lkpi_bio_set *bs)
{
	unsigned int pad = bioset_front_pad(bs);
	usize size = pad + sizeof(struct lkpi_bio) +
	             (usize)nr_vecs * sizeof(struct bio_vec);
	char *base = lkpi_kmalloc(size, 0);
	struct lkpi_bio *bio;

	if (!base)
		return NULL;
	memset(base, 0, size);
	bio = (struct lkpi_bio *)(base + pad);
	bio->bi_bdev = bdev;
	bio->bi_opf = opf;
	bio->bi_max_vecs = nr_vecs;
	bio->bi_io_vec = bio->bi_inline_vecs;
	/* Remembered so the free can find the allocation's base again. */
	bio->bi_pool = bs;
	bio->__bi_cnt = 1;
	/*
	 * One outstanding piece: the bio itself. Chaining a child raises it, and
	 * each completion lowers it — so the parent's end_io runs when the count
	 * reaches zero and not before. Starting at zero would complete the parent
	 * on the first child's completion.
	 */
	bio->__bi_remaining = 1;
	return bio;
}

struct lkpi_bio *bio_alloc_bioset(struct lkpi_block_device *bdev,
                                  unsigned short nr_vecs, unsigned int opf,
                                  u32 gfp_mask, void *bs)
{
	(void)gfp_mask;
	return bio_alloc_common(bdev, nr_vecs, opf, bs);
}

struct lkpi_bio *bio_kmalloc(unsigned short nr_vecs, u32 gfp_mask)
{
	(void)gfp_mask;
	return bio_alloc_common(NULL, nr_vecs, 0, NULL);
}

void bio_init(struct lkpi_bio *bio, struct lkpi_block_device *bdev,
              struct bio_vec *table, unsigned short max_vecs, unsigned int opf)
{
	memset(bio, 0, sizeof(*bio));
	bio->bi_bdev = bdev;
	bio->bi_opf = opf;
	bio->bi_io_vec = table;
	bio->bi_max_vecs = max_vecs;
	bio->__bi_cnt = 1;
	bio->__bi_remaining = 1;
}

void bio_uninit(struct lkpi_bio *bio) { (void)bio; }

void bio_reset(struct lkpi_bio *bio, struct lkpi_block_device *bdev,
               unsigned int opf)
{
	struct bio_vec *table = bio->bi_io_vec;
	unsigned short max = bio->bi_max_vecs;

	memset(bio, 0, sizeof(*bio));
	bio->bi_io_vec = table;
	bio->bi_max_vecs = max;
	bio->bi_bdev = bdev;
	bio->bi_opf = opf;
	bio->__bi_cnt = 1;
	bio->__bi_remaining = 1;
}

void bio_put(struct lkpi_bio *bio)
{
	if (!bio)
		return;
	if (__atomic_sub_fetch(&bio->__bi_cnt, 1, __ATOMIC_ACQ_REL) > 0)
		return;
	/*
	 * A bio whose vector is its own inline array is one allocation; one that
	 * was initialised over a caller's storage is not ours to free, and is
	 * distinguished by exactly that.
	 *
	 * The allocation starts `front_pad` bytes before the bio — see
	 * bio_alloc_common — so the free has to go back by the same amount, or it
	 * hands the heap a pointer into the middle of a block.
	 */
	if (bio->bi_io_vec == bio->bi_inline_vecs) {
		unsigned int pad = bioset_front_pad(bio->bi_pool);

		lkpi_kfree((char *)bio - pad);
	}
}

struct lkpi_bio *bio_alloc_clone(struct lkpi_block_device *bdev,
                                 struct lkpi_bio *bio_src, u32 gfp, void *bs)
{
	struct lkpi_bio *bio;

	(void)gfp;
	(void)bs;
	bio = bio_alloc_common(bdev ? bdev : bio_src->bi_bdev, bio_src->bi_vcnt,
	                       bio_src->bi_opf, bs);
	if (!bio)
		return NULL;
	/* The clone shares the ORIGINAL's pages — it is a second description of
	 * the same bytes, not a copy of them. */
	memcpy(bio->bi_io_vec, bio_src->bi_io_vec,
	       (usize)bio_src->bi_vcnt * sizeof(struct bio_vec));
	bio->bi_vcnt = bio_src->bi_vcnt;
	bio->bi_iter = bio_src->bi_iter;
	return bio;
}

/* ── building ───────────────────────────────────────────────────── */

void __bio_add_page(struct lkpi_bio *bio, struct page *page, unsigned int len,
                    unsigned int off)
{
	struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt++];

	bv->bv_page = page;
	bv->bv_len = len;
	bv->bv_offset = off;
	bio->bi_iter.bi_size += len;
}

int bio_add_page(struct lkpi_bio *bio, struct page *page, unsigned int len,
                 unsigned int off)
{
	if (bio->bi_vcnt >= bio->bi_max_vecs)
		return 0;
	/* Merge with the previous entry when it is the same page and the runs are
	 * adjacent — which is what a page-at-a-time caller produces, and what
	 * would otherwise fill the vector at one entry per block. */
	if (bio->bi_vcnt > 0) {
		struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt - 1];

		if (bv->bv_page == page && bv->bv_offset + bv->bv_len == off) {
			bv->bv_len += len;
			bio->bi_iter.bi_size += len;
			return (int)len;
		}
	}
	__bio_add_page(bio, page, len, off);
	return (int)len;
}

/* A folio is one page here (see <linux/mm.h>), so both forms add one entry. */
int bio_add_folio(struct lkpi_bio *bio, void *folio, usize len, usize off)
{
	return bio_add_page(bio, (struct page *)folio, (unsigned int)len,
	                    (unsigned int)off) != 0;
}

void bio_add_folio_nofail(struct lkpi_bio *bio, void *folio, usize len,
                          usize off)
{
	__bio_add_page(bio, (struct page *)folio, (unsigned int)len,
	               (unsigned int)off);
}

void bio_set_dev(struct lkpi_bio *bio, struct lkpi_block_device *bdev)
{
	bio->bi_bdev = bdev;
}

/*
 * Consume `bytes` from an iterator.
 *
 * Both halves matter: `bi_size` shrinks so the caller knows how much is left,
 * and `bi_sector` advances so the next submission starts where this one
 * stopped. Advancing one without the other rewrites the same sectors or skips
 * them.
 */
void bio_advance_iter_single(const struct lkpi_bio *bio, struct bvec_iter *iter,
                             unsigned int bytes)
{
	(void)bio;
	iter->bi_sector += bytes >> LKPI_SECTOR_SHIFT;
	if (bytes >= iter->bi_size) {
		iter->bi_size = 0;
		return;
	}
	iter->bi_size -= bytes;
	iter->bi_bvec_done += bytes;
	while (iter->bi_idx < (unsigned int)-1) {
		const struct bio_vec *bv = &bio->bi_io_vec[iter->bi_idx];

		if (iter->bi_bvec_done < bv->bv_len)
			break;
		iter->bi_bvec_done -= bv->bv_len;
		iter->bi_idx++;
	}
}

void bio_advance(struct lkpi_bio *bio, unsigned int nbytes)
{
	bio_advance_iter_single(bio, &bio->bi_iter, nbytes);
}

/* The current entry, clipped to what is left of the I/O. */
struct bio_vec bio_iter_iovec(struct lkpi_bio *bio, struct bvec_iter iter)
{
	struct bio_vec bv = bio->bi_io_vec[iter.bi_idx];

	bv.bv_offset += iter.bi_bvec_done;
	bv.bv_len -= iter.bi_bvec_done;
	if (bv.bv_len > iter.bi_size)
		bv.bv_len = iter.bi_size;
	return bv;
}

/*
 * Zero what is left of the I/O, from the bio's current iterator on.
 *
 * Not the whole vector: a decompressing reader (btrfs) advances the iterator
 * past the bytes it filled and then calls this for the tail the extent did not
 * cover. Zeroing from entry 0 wiped every byte it had just decompressed, so a
 * compressed file read back as zeros without any error.
 */
void zero_fill_bio(struct lkpi_bio *bio)
{
	struct bvec_iter iter = bio->bi_iter;

	while (iter.bi_size) {
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		memset((char *)page_address(bv.bv_page) + bv.bv_offset, 0,
		       bv.bv_len);
		bio_advance_iter_single(bio, &iter, bv.bv_len);
	}
}

/* ── completion and chaining ────────────────────────────────────── */

/* BIO_CHAIN's bit in bi_flags (enum order in <linux/blk_types.h>). */
#define LKPI_BIO_CHAIN 4

void bio_inc_remaining(struct lkpi_bio *bio)
{
	bio->bi_flags |= (1U << LKPI_BIO_CHAIN);
	__atomic_fetch_add(&bio->__bi_remaining, 1, __ATOMIC_ACQ_REL);
}

static void bio_chain_endio(struct lkpi_bio *bio)
{
	struct lkpi_bio *parent = bio->bi_private;

	/* A child's failure is the parent's failure: the parent describes an I/O
	 * that is only complete if every piece of it succeeded. */
	if (bio->bi_status && !parent->bi_status)
		parent->bi_status = bio->bi_status;
	bio_put(bio);
	/* The parent's own end_io runs from here, once nothing is outstanding. */
	extern void bio_endio(struct lkpi_bio *bio);
	bio_endio(parent);
}

void bio_chain(struct lkpi_bio *bio, struct lkpi_bio *parent)
{
	bio->bi_private = parent;
	bio->bi_end_io = bio_chain_endio;
	bio_inc_remaining(parent);
}

void bio_endio(struct lkpi_bio *bio)
{
	if (!bio)
		return;
	/*
	 * Not finished while a chained child is still outstanding -- but the
	 * count means something only on a bio that was chained, as upstream's
	 * bio_remaining_done has it. Decrementing it on every call broke a bio
	 * whose completion runs bio_endio more than once: the count went through
	 * zero, end_io ran twice, the bio was put twice, and the second
	 * decrement landed in a freed btrfs_bio.
	 */
	if (bio->bi_flags & (1U << LKPI_BIO_CHAIN)) {
		if (__atomic_sub_fetch(&bio->__bi_remaining, 1, __ATOMIC_ACQ_REL) > 0)
			return;
		bio->bi_flags &= (unsigned short)~(1U << LKPI_BIO_CHAIN);
	}
	if (bio->bi_end_io)
		bio->bi_end_io(bio);
}

struct lkpi_bio *bio_split(struct lkpi_bio *bio, int sectors, u32 gfp, void *bs)
{
	struct lkpi_bio *split;

	/*
	 * From the caller's bioset, not the default one. btrfs splits into its
	 * own set, whose front_pad puts a `struct btrfs_bio` in front of the
	 * bio; allocating without it puts btrfs_bio(bio) before the allocation
	 * and every field it writes lands in the heap's own bookkeeping.
	 */
	split = bio_alloc_clone(bio->bi_bdev, bio, gfp, bs);
	if (!split)
		return NULL;
	split->bi_iter.bi_size = (unsigned int)sectors << LKPI_SECTOR_SHIFT;
	/* The remainder stays in the original, starting where the split ends —
	 * which is what makes the pair describe the same bytes exactly once. */
	bio_advance(bio, split->bi_iter.bi_size);
	/*
	 * Deliberately NOT chained. Upstream's bio_split does not chain either:
	 * the caller owns both completions, and btrfs replaces the end_io on
	 * both halves. Chaining here raised the parent's outstanding count and
	 * installed an end_io that btrfs then overwrote, so the parent's
	 * completion could never arrive.
	 */
	return split;
}

struct lkpi_bio *bio_split_rw(struct lkpi_bio *bio, const void *lim,
                              unsigned *segs, void *bs, unsigned max_bytes)
{
	(void)lim;
	(void)bs;
	(void)max_bytes;
	if (segs)
		*segs = bio->bi_vcnt;
	/*
	 * NULL means "the whole bio fits", and here it always does: b1nix's block
	 * cache takes an arbitrary length and does its own chopping at the
	 * device's limits. Splitting here as well would be a second, worse
	 * implementation of that.
	 */
	return NULL;
}

/* ── submission ─────────────────────────────────────────────────── */

/*
 * Turn one bio into calls on b1nix's block cache.
 *
 * The conversion from sectors to the device's own blocks happens here and
 * nowhere else. A 512-byte sector count divided by a 4096-byte block size is
 * eight sectors per block, and a bio that starts mid-block is not something
 * this path can express — a filesystem always submits block-aligned I/O, so
 * that is checked rather than handled.
 */
static int bio_submit_range(struct block_device *dev, unsigned int op,
                            u64 sector, void *buf, unsigned int len)
{
	usize block_size = dev->block_size ? dev->block_size : 512;
	u64 byte_off = sector << LKPI_SECTOR_SHIFT;
	u64 lba;
	u32 count;

	if (block_size == 0 || (byte_off % block_size) != 0 ||
	    (len % block_size) != 0) {
		klog_error("lkpi bio: submission not aligned to the device's block "
		           "size; a filesystem must not issue one");
		return -1;
	}
	lba = byte_off / block_size;
	count = (u32)(len / block_size);
	if (op == LKPI_REQ_OP_READ)
		return blk_read_cached(dev, lba, count, buf);
	return blk_write_cached(dev, lba, count, buf);
}

void submit_bio(struct lkpi_bio *bio)
{
	struct block_device *dev;
	unsigned int op;
	u64 sector;
	unsigned short i;
	int failed = 0;

	if (!bio)
		return;
	op = bio->bi_opf & LKPI_REQ_OP_MASK;
	dev = bio->bi_bdev ? bio->bi_bdev->bd_b1nix : NULL;

	if (!dev) {
		bio->bi_status = LKPI_BLK_STS_IOERR;
		bio_endio(bio);
		return;
	}

	switch (op) {
	case LKPI_REQ_OP_FLUSH:
		/* A flush carries no data: it orders everything already written. */
		blk_cache_flush(dev);
		bio_endio(bio);
		return;
	case LKPI_REQ_OP_DISCARD:
		if (blk_discard_supported(dev)) {
			usize bs = dev->block_size ? dev->block_size : 512;
			u64 lba = (bio->bi_iter.bi_sector << LKPI_SECTOR_SHIFT) / bs;
			u32 count = (u32)(bio->bi_iter.bi_size / bs);

			if (blk_discard_blocks(dev, lba, count) != 0)
				bio->bi_status = LKPI_BLK_STS_IOERR;
		} else {
			/* Not an error: a discard is advice, and a device that cannot
			 * take it has lost nothing. Reporting NOTSUPP would make a
			 * filesystem log a failure for a hint it never needed. */
			bio->bi_status = LKPI_BLK_STS_OK;
		}
		bio->bi_iter.bi_size = 0;
		bio_endio(bio);
		return;
	case LKPI_REQ_OP_WRITE_ZEROES: {
		usize bs = dev->block_size ? dev->block_size : 512;
		u64 lba = (bio->bi_iter.bi_sector << LKPI_SECTOR_SHIFT) / bs;
		u32 count = (u32)(bio->bi_iter.bi_size / bs);

		if (blk_zero_blocks(dev, lba, count) != 0)
			bio->bi_status = LKPI_BLK_STS_IOERR;
		bio->bi_iter.bi_size = 0;
		bio_endio(bio);
		return;
	}
	default:
		break;
	}

	/* The write-back block cache sits between this and the medium, and a
	 * journal's safety is entirely in these two flags. jbd2 writes its commit
	 * block, and later the journal superblock's new tail, with PREFLUSH|FUA:
	 * everything before must be on the medium first, and this block must be
	 * there when the bio completes. Ignored, the cache wrote back in LBA
	 * order, and a phone reset left a commit without its transaction or a
	 * tail moved past blocks never checkpointed -- block bitmaps that
	 * disagreed with their descriptors, and files that vanished on replay. */
	if (op == LKPI_REQ_OP_WRITE && (bio->bi_opf & LKPI_REQ_PREFLUSH))
		blk_cache_flush(dev);

	/*
	 * Walk the vector from where the iterator has got to, not from entry
	 * zero: a split bio shares its parent's vector, and starting at zero
	 * would transfer the parent's first bytes to the child's sectors.
	 */
	sector = bio->bi_iter.bi_sector;
	for (i = (unsigned short)bio->bi_iter.bi_idx;
	     i < bio->bi_vcnt && bio->bi_iter.bi_size; i++) {
		struct bio_vec *bv = &bio->bi_io_vec[i];
		unsigned int off = bv->bv_offset;
		unsigned int len = bv->bv_len;
		char *addr;

		if (i == (unsigned short)bio->bi_iter.bi_idx) {
			off += bio->bi_iter.bi_bvec_done;
			len -= bio->bi_iter.bi_bvec_done;
		}
		if (len > bio->bi_iter.bi_size)
			len = bio->bi_iter.bi_size;
		addr = (char *)page_address(bv->bv_page) + off;
		if (bio_submit_range(dev, op, sector, addr, len) != 0) {
			failed = 1;
			break;
		}
		sector += len >> LKPI_SECTOR_SHIFT;
		bio->bi_iter.bi_size -= len;
	}

	if (!failed && op == LKPI_REQ_OP_WRITE && (bio->bi_opf & LKPI_REQ_FUA))
		blk_cache_flush(dev);

	bio->bi_iter.bi_sector = sector;
	bio->bi_status = failed ? LKPI_BLK_STS_IOERR : LKPI_BLK_STS_OK;
	bio_endio(bio);
}

/*
 * Submit and wait.
 *
 * b1nix's block cache completes synchronously, so the wait is already over by
 * the time submit_bio returns — but the interface still has to report the
 * status, because that is what every caller checks. When the block layer grows
 * an asynchronous path, this is the function that parks.
 */
int submit_bio_wait(struct lkpi_bio *bio)
{
	submit_bio(bio);
	return bio->bi_status ? -5 /* -EIO */ : 0;
}

int blk_status_to_errno(u8 status)
{
	switch (status) {
	case LKPI_BLK_STS_OK:
		return 0;
	case LKPI_BLK_STS_NOTSUPP:
		return -95;  /* -EOPNOTSUPP */
	case 3:
		return -28;  /* -ENOSPC */
	case 9:
		return -11;  /* -EAGAIN, from BLK_STS_RESOURCE */
	default:
		return -5;   /* -EIO */
	}
}

u8 errno_to_blk_status(int err)
{
	switch (err) {
	case 0:
		return LKPI_BLK_STS_OK;
	case -95:
		return LKPI_BLK_STS_NOTSUPP;
	case -28:
		return 3;
	case -11:
		return 9;
	default:
		return LKPI_BLK_STS_IOERR;
	}
}

/* ── bio sets and plugs ─────────────────────────────────────────── */

/*
 * A bio set is a reserve upstream, so that a path which must make progress can
 * always allocate. b1nix's kmalloc does not fail under pressure — it panics
 * when the heap cannot grow — so there is nothing to reserve against and the
 * set records its sizes and nothing else. See the note in <linux/bio.h>.
 */
int bioset_init(struct lkpi_bio_set *bs, unsigned int pool_size,
                unsigned int front_pad, int flags)
{
	(void)pool_size;
	(void)flags;
	bs->front_pad = front_pad;
	bs->initialised = 1;
	return 0;
}

void bioset_exit(struct lkpi_bio_set *bs)
{
	bs->initialised = 0;
}

struct lkpi_bio_set fs_bio_set;

/* A plug batches submissions so the layer below can merge them. b1nix's block
 * cache merges on its own and has no per-task list, so both calls are
 * structural — see <linux/blkdev.h>. */
void blk_start_plug(void *plug) { (void)plug; }
void blk_finish_plug(void *plug) { (void)plug; }
void blk_flush_plug(void *plug, int from_schedule)
{ (void)plug; (void)from_schedule; }
void *blk_check_plugged(void *unplug, void *data, int size)
{ (void)unplug; (void)data; (void)size; return NULL; }

/* Park while an I/O this task submitted is outstanding. There is no I/O
 * scheduler to hand the CPU to, and completions here are synchronous, so this
 * is a plain yield. */
void blk_io_schedule(void) { scheduler_yield(); }
void blk_wake_io_task(void *waiter) { (void)waiter; }

/* ── the b1nix side of the device bridge ────────────────────────── */

/*
 * kernel/lkpi/fs_bdev.c pairs a Linux `struct block_device` with one of these,
 * and asks its questions through the wrappers below. They exist because that
 * file cannot include <b1nix/blk.h> — it already has the Linux structure of the
 * same name — so every reach into b1nix's block layer goes through a function
 * whose name cannot collide.
 */

struct block_device *lkpi_blk_get(const char *name)
{
	return name ? blk_get(name) : NULL;
}

struct block_device *lkpi_blk_from_devno(u64 rdev)
{
	return blk_from_devno(rdev);
}

u64 lkpi_blk_block_count(struct block_device *dev)
{
	return dev ? dev->block_count : 0;
}

unsigned int lkpi_blk_block_size(struct block_device *dev)
{
	return dev ? (unsigned int)dev->block_size : 0;
}

const char *lkpi_blk_name(struct block_device *dev)
{
	return dev ? dev->name : NULL;
}

u32 lkpi_blk_devno(struct block_device *dev)
{
	return dev ? blk_devno(dev) : 0;
}

int lkpi_blk_is_rotational(struct block_device *dev)
{
	return dev ? dev->rotational : 0;
}

int lkpi_blk_discard_supported(struct block_device *dev)
{
	return dev ? blk_discard_supported(dev) : 0;
}

int lkpi_blk_discard(struct block_device *dev, u64 lba, u32 count)
{
	/* Through blk_discard_blocks, never the driver's own hook: the cached
	 * copies of those blocks have to be dropped first, and going straight to
	 * the device would leave them behind to be written back over the hole. */
	return dev ? blk_discard_blocks(dev, lba, count) : -1;
}

int lkpi_blk_zero(struct block_device *dev, u64 lba, u32 count)
{
	return dev ? blk_zero_blocks(dev, lba, count) : -1;
}

void lkpi_blk_flush(struct block_device *dev)
{
	if (dev)
		blk_cache_flush(dev);
}

void lkpi_blk_invalidate(struct block_device *dev)
{
	if (dev)
		blk_cache_invalidate(dev);
}

u32 lkpi_blk_max_sectors(struct block_device *dev)
{
	/*
	 * The device's own per-command ceiling, in 512-byte sectors. blk_max_sectors
	 * answers in the device's blocks, so it is converted here — reporting the
	 * block count as a sector count would let a filesystem build requests
	 * eight times too large on a 4 KiB device.
	 */
	u32 blocks;
	usize bs;

	if (!dev)
		return 2560;
	blocks = blk_max_sectors(dev);
	bs = dev->block_size ? dev->block_size : 512;
	return (u32)((u64)blocks * bs / 512u);
}
