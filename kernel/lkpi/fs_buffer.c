/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: buffer heads.
 *
 * The layer ext4 and jbd2 are built on, and the one btrfs does not use at all —
 * which is why it could be left out while btrfs was brought up. A buffer head
 * is a block-sized window onto a page-cache folio, plus the state that says
 * whether those bytes are current, dirty, or under I/O.
 *
 * Two kinds of folio carry buffers here, and the difference matters:
 *
 *   - a folio of the block DEVICE's mapping, for metadata. `__getblk(bdev, n)`
 *     is "the n-th block of this device", and its buffer's block number is
 *     known the moment it is created.
 *   - a folio of a FILE's mapping, for data. Those buffers start unmapped, and
 *     the filesystem's own get_block() is what assigns each one a block on the
 *     device — which is where a hole becomes an allocation.
 *
 * Reads and writes go out as ordinary bios, so they land in b1nix's block cache
 * like everything else and a filesystem's metadata is coherent with a read of
 * the raw device.
 */

#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/highmem.h>
#include <linux/err.h>
#include <linux/printk.h>
#include <lkpi/env.h>

/* ── the buffer itself ──────────────────────────────────────────── */

struct buffer_head *alloc_buffer_head(gfp_t gfp_flags)
{
	struct buffer_head *bh = kzalloc(sizeof(*bh), gfp_flags);

	if (!bh)
		return NULL;
	INIT_LIST_HEAD(&bh->b_assoc_buffers);
	spin_lock_init(&bh->b_uptodate_lock);
	atomic_set(&bh->b_count, 0);
	return bh;
}

void free_buffer_head(struct buffer_head *bh)
{
	kfree(bh);
}

/*
 * The lock is a bit, and a waiter parks on the buffer's own address.
 *
 * Upstream's is the same bit with a folio-wide wait queue; the channel here is
 * the buffer, which is finer and needs no table. What must not change is that
 * unlock wakes: a filesystem waits for a metadata read by waiting for the
 * buffer to unlock, and a missed wake there is a mount that never finishes.
 */
void lock_buffer(struct buffer_head *bh)
{
	while (test_and_set_bit(BH_Lock, &bh->b_state)) {
		lkpi_wait_prepare(bh);
		if (!test_bit(BH_Lock, &bh->b_state)) {
			lkpi_wait_cancel();
			continue;
		}
		lkpi_wait_commit();
	}
}

int trylock_buffer(struct buffer_head *bh)
{
	return !test_and_set_bit(BH_Lock, &bh->b_state);
}

void unlock_buffer(struct buffer_head *bh)
{
	clear_bit(BH_Lock, &bh->b_state);
	lkpi_wake_all(bh);
}

void wait_on_buffer(struct buffer_head *bh)
{
	while (test_bit(BH_Lock, &bh->b_state)) {
		lkpi_wait_prepare(bh);
		if (!test_bit(BH_Lock, &bh->b_state)) {
			lkpi_wait_cancel();
			return;
		}
		lkpi_wait_commit();
	}
}

int bh_uptodate_or_lock(struct buffer_head *bh)
{
	if (!buffer_uptodate(bh)) {
		lock_buffer(bh);
		if (!buffer_uptodate(bh))
			return 0;
		unlock_buffer(bh);
	}
	return 1;
}

void __brelse(struct buffer_head *bh)
{
	if (atomic_read(&bh->b_count) > 0)
		atomic_dec(&bh->b_count);
}

void __bforget(struct buffer_head *bh)
{
	/* "Forget" is a release that also drops any dirty state: the caller is
	 * saying the contents are no longer wanted, and writing them back after
	 * that would put stale bytes on the disk. */
	clear_buffer_dirty(bh);
	if (!list_empty(&bh->b_assoc_buffers))
		list_del_init(&bh->b_assoc_buffers);
	bh->b_assoc_map = NULL;
	__brelse(bh);
}

/* ── buffers on a folio ─────────────────────────────────────────── */

struct buffer_head *folio_buffers(struct folio *folio)
{
	if (!folio || !folio_test_private(folio))
		return NULL;
	return (struct buffer_head *)folio_get_private(folio);
}

void folio_set_bh(struct buffer_head *bh, struct folio *folio,
                  unsigned long offset)
{
	bh->b_folio = folio;
	bh->b_data = (char *)folio_address(folio) + offset;
}

/*
 * Divide a folio into buffers of `blocksize` and hang the ring off it.
 *
 * Idempotent: a folio that already has buffers keeps them, because two callers
 * racing here would otherwise each build a ring and the loser's buffers would
 * be the ones the filesystem is holding.
 */
static struct buffer_head *folio_create_buffers(struct folio *folio,
                                                unsigned long blocksize,
                                                unsigned long state)
{
	struct buffer_head *head = folio_buffers(folio);
	struct buffer_head *bh, *tail = NULL;
	unsigned long offset;

	if (head)
		return head;
	for (offset = 0; offset < PAGE_SIZE; offset += blocksize) {
		bh = alloc_buffer_head(GFP_NOFS);
		if (!bh)
			goto fail;
		bh->b_state = state;
		bh->b_size = blocksize;
		folio_set_bh(bh, folio, offset);
		if (!head)
			head = bh;
		else
			tail->b_this_page = bh;
		tail = bh;
	}
	if (tail)
		tail->b_this_page = head;   /* a ring, as upstream's is */
	folio_attach_private(folio, head);
	return head;

fail:
	while (head) {
		bh = head->b_this_page;
		free_buffer_head(head);
		head = (bh == head) ? NULL : bh;
	}
	return NULL;
}

void create_empty_buffers(struct page *page, unsigned long blocksize,
                          unsigned long b_state)
{
	struct folio *folio = page_folio(page);
	struct buffer_head *head = folio_create_buffers(folio, blocksize, b_state);
	struct buffer_head *bh = head;

	/* A folio that is already up to date has up-to-date buffers: its bytes
	 * are the file's bytes, whoever put them there. */
	if (head && folio_test_uptodate(folio)) {
		do {
			set_buffer_uptodate(bh);
			bh = bh->b_this_page;
		} while (bh && bh != head);
	}
}

int try_to_free_buffers(struct folio *folio)
{
	struct buffer_head *head = folio_buffers(folio);
	struct buffer_head *bh = head;

	if (!head)
		return 1;
	/* A buffer that is dirty, locked or held is in use by somebody; freeing
	 * the ring under them would be a use-after-free, and losing a dirty
	 * buffer would be a lost write. */
	do {
		if (buffer_dirty(bh) || buffer_locked(bh) ||
		    atomic_read(&bh->b_count) > 0)
			return 0;
		bh = bh->b_this_page;
	} while (bh && bh != head);

	folio_detach_private(folio);
	bh = head;
	do {
		struct buffer_head *next = bh->b_this_page;

		free_buffer_head(bh);
		bh = (next == head) ? NULL : next;
	} while (bh);
	return 1;
}

/* ── metadata buffers, addressed by device block ────────────────── */

/*
 * The folio of the block device that holds device block `block` of `size`.
 *
 * The device's mapping is the one thing that makes two names for the same
 * block the same bytes: a filesystem that read its superblock through
 * `read_cache_page_gfp` and its group descriptors through `__getblk` must see
 * one copy, not two.
 */
static struct folio *bdev_folio_for(struct block_device *bdev, sector_t block,
                                    unsigned size, unsigned long *offset)
{
	unsigned long long byte = (unsigned long long)block * size;
	pgoff_t index = (pgoff_t)(byte >> PAGE_SHIFT);
	struct folio *folio;

	if (!bdev || !bdev->bd_inode || size == 0 || size > PAGE_SIZE)
		return NULL;
	folio = __filemap_get_folio(bdev->bd_inode->i_mapping, index,
	                            FGP_LOCK | FGP_CREAT, GFP_NOFS);
	if (IS_ERR(folio) || !folio)
		return NULL;
	*offset = (unsigned long)(byte & (PAGE_SIZE - 1));
	return folio;
}

static struct buffer_head *bh_in_folio(struct folio *folio, unsigned long offset,
                                       unsigned size)
{
	struct buffer_head *head = folio_buffers(folio);
	struct buffer_head *bh = head;

	if (!head)
		return NULL;
	do {
		if (bh->b_size == size && bh_offset(bh) == offset)
			return bh;
		bh = bh->b_this_page;
	} while (bh && bh != head);
	return NULL;
}

static struct buffer_head *getblk_common(struct block_device *bdev,
                                         sector_t block, unsigned size,
                                         int create)
{
	unsigned long offset = 0;
	struct folio *folio = bdev_folio_for(bdev, block, size, &offset);
	struct buffer_head *bh;

	if (!folio)
		return NULL;
	if (!folio_buffers(folio)) {
		if (!create) {
			folio_unlock(folio);
			folio_put(folio);
			return NULL;
		}
		if (!folio_create_buffers(folio, size, 0)) {
			folio_unlock(folio);
			folio_put(folio);
			return NULL;
		}
	}
	bh = bh_in_folio(folio, offset, size);
	if (!bh) {
		/*
		 * The folio is already divided a different way. Upstream grows a new
		 * page for the new size; here the mismatch means two block sizes on
		 * one device, which neither filesystem does after mount — and
		 * silently returning a buffer of the wrong size would corrupt it.
		 */
		folio_unlock(folio);
		folio_put(folio);
		return NULL;
	}
	if (!bh->b_bdev) {
		bh->b_bdev = bdev;
		bh->b_blocknr = block;
		set_buffer_mapped(bh);
		/* The device's own folio may already hold the bytes — it is the same
		 * cache a read of the raw device fills. */
		if (folio_test_uptodate(folio))
			set_buffer_uptodate(bh);
	}
	atomic_inc(&bh->b_count);
	folio_unlock(folio);
	/* The folio reference stays: the buffer points into it, so it must not be
	 * reclaimed while the buffer is held. It is released when the mapping is
	 * invalidated at unmount. */
	return bh;
}

struct buffer_head *__getblk(struct block_device *bdev, sector_t block,
                             unsigned size)
{
	return getblk_common(bdev, block, size, 1);
}

struct buffer_head *__getblk_gfp(struct block_device *bdev, sector_t block,
                                 unsigned size, gfp_t gfp)
{
	(void)gfp;
	return getblk_common(bdev, block, size, 1);
}

struct buffer_head *__find_get_block(struct block_device *bdev, sector_t block,
                                     unsigned size)
{
	return getblk_common(bdev, block, size, 0);
}

/* ── I/O ────────────────────────────────────────────────────────── */

void end_buffer_read_sync(struct buffer_head *bh, int uptodate)
{
	if (uptodate)
		set_buffer_uptodate(bh);
	else
		clear_buffer_uptodate(bh);
	unlock_buffer(bh);
	__brelse(bh);
}

void end_buffer_write_sync(struct buffer_head *bh, int uptodate)
{
	if (!uptodate) {
		mark_buffer_write_io_error(bh);
		clear_buffer_uptodate(bh);
	}
	unlock_buffer(bh);
	__brelse(bh);
}

void end_buffer_async_write(struct buffer_head *bh, int uptodate)
{
	end_buffer_write_sync(bh, uptodate);
}

/*
 * One buffer, one bio.
 *
 * b1nix's block layer completes synchronously (see kernel/lkpi/bio.c), so the
 * completion has already run by the time submit_bio returns — but the
 * end_io/unlock protocol is upstream's, because a caller that waits on the
 * buffer must be woken whether the wait was needed or not.
 */
void submit_bh(blk_opf_t opf, struct buffer_head *bh)
{
	unsigned int op = opf & REQ_OP_MASK;

	struct bio *bio;

	if (!bh || !bh->b_bdev || !bh->b_data) {
		if (bh) {
			if (bh->b_end_io)
				bh->b_end_io(bh, 0);
			else
				unlock_buffer(bh);
		}
		return;
	}

	bio = bio_alloc(bh->b_bdev, 1, op, GFP_NOFS);
	if (!bio) {
		if (bh->b_end_io)
			bh->b_end_io(bh, 0);
		else
			unlock_buffer(bh);
		return;
	}
	bio->bi_iter.bi_sector = (sector_t)(bh->b_blocknr *
	                                    (bh->b_size >> SECTOR_SHIFT));
	bio_add_page(bio, bh->b_page, (unsigned)bh->b_size, bh_offset(bh));

	if (op == REQ_OP_WRITE)
		clear_buffer_dirty(bh);

	if (submit_bio_wait(bio) == 0) {
		if (op != REQ_OP_WRITE)
			set_buffer_uptodate(bh);
		else
			set_buffer_uptodate(bh);
		bio_put(bio);
		if (bh->b_end_io)
			bh->b_end_io(bh, 1);
		else
			unlock_buffer(bh);
		return;
	}
	bio_put(bio);
	if (op == REQ_OP_WRITE)
		set_buffer_dirty(bh);   /* it did not reach the disk; it is still dirty */
	if (bh->b_end_io)
		bh->b_end_io(bh, 0);
	else
		unlock_buffer(bh);
}

int bh_read_nowait(struct buffer_head *bh, blk_opf_t op_flags)
{
	if (buffer_uptodate(bh))
		return 1;
	if (!trylock_buffer(bh))
		return 0;
	if (buffer_uptodate(bh)) {
		unlock_buffer(bh);
		return 1;
	}
	get_bh(bh);
	bh->b_end_io = end_buffer_read_sync;
	submit_bh(REQ_OP_READ | op_flags, bh);
	return 0;
}

int bh_read(struct buffer_head *bh, blk_opf_t op_flags)
{
	if (buffer_uptodate(bh))
		return 1;
	lock_buffer(bh);
	if (buffer_uptodate(bh)) {
		unlock_buffer(bh);
		return 1;
	}
	get_bh(bh);
	bh->b_end_io = end_buffer_read_sync;
	submit_bh(REQ_OP_READ | op_flags, bh);
	wait_on_buffer(bh);
	return buffer_uptodate(bh) ? 1 : -EIO;
}

void bh_readahead(struct buffer_head *bh, blk_opf_t op_flags)
{
	bh_read_nowait(bh, op_flags);
}

void bh_readahead_batch(int nr, struct buffer_head *bhs[], blk_opf_t op_flags)
{
	int i;

	for (i = 0; i < nr; i++)
		bh_read_nowait(bhs[i], op_flags);
}

void __bh_read_batch(int nr, struct buffer_head *bhs[], blk_opf_t op_flags,
                     bool force_lock)
{
	int i;

	(void)force_lock;
	for (i = 0; i < nr; i++)
		bh_read(bhs[i], op_flags);
}

struct buffer_head *__bread_gfp(struct block_device *bdev, sector_t block,
                                unsigned size, gfp_t gfp)
{
	struct buffer_head *bh = __getblk_gfp(bdev, block, size, gfp);

	if (!bh)
		return NULL;
	if (buffer_uptodate(bh))
		return bh;
	if (bh_read(bh, 0) < 0) {
		__brelse(bh);
		return NULL;
	}
	return bh;
}

struct buffer_head *__bread(struct block_device *bdev, sector_t block,
                            unsigned size)
{
	return __bread_gfp(bdev, block, size, 0);
}

void sb_breadahead(struct super_block *sb, sector_t block)
{
	struct buffer_head *bh = sb_getblk(sb, block);

	if (bh) {
		bh_readahead(bh, 0);
		__brelse(bh);
	}
}

/* ── dirtying and writing back ──────────────────────────────────── */

void mark_buffer_dirty(struct buffer_head *bh)
{
	if (!bh)
		return;
	set_buffer_dirty(bh);
	/*
	 * And the folio, so that a writeback pass over the mapping finds it. A
	 * buffer marked dirty on a clean folio is invisible to every walk that
	 * starts from the mapping, which is how metadata gets left behind.
	 */
	if (bh->b_folio)
		folio_mark_dirty(bh->b_folio);
}

void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode)
{
	mark_buffer_dirty(bh);
	if (inode)
		bh->b_assoc_map = inode->i_mapping;
}

void mark_buffer_write_io_error(struct buffer_head *bh)
{
	set_buffer_write_io_error(bh);
	if (bh->b_assoc_map)
		mapping_set_error(bh->b_assoc_map, -EIO);
}

void write_dirty_buffer(struct buffer_head *bh, blk_opf_t op_flags)
{
	if (!bh)
		return;
	lock_buffer(bh);
	if (!test_clear_buffer_dirty(bh)) {
		unlock_buffer(bh);
		return;
	}
	get_bh(bh);
	bh->b_end_io = end_buffer_write_sync;
	submit_bh(REQ_OP_WRITE | op_flags, bh);
}

int sync_dirty_buffer(struct buffer_head *bh)
{
	if (!bh)
		return -EINVAL;
	write_dirty_buffer(bh, REQ_SYNC);
	wait_on_buffer(bh);
	return buffer_uptodate(bh) ? 0 : -EIO;
}

void invalidate_bh_lrus(void) { }
void invalidate_bh_lrus_cpu(void) { }

/*
 * Write out every dirty buffer of a mapping.
 *
 * This is what makes a metadata write reach the disk: the device's mapping has
 * no writeback of its own, and a filesystem that marked a buffer dirty and then
 * synced the device expects the bytes to be there afterwards.
 */
int lkpi_write_dirty_buffers(struct address_space *mapping)
{
	struct folio_batch fbatch;
	pgoff_t index = 0;
	int written = 0;

	if (!mapping)
		return 0;
	folio_batch_init(&fbatch);
	while (filemap_get_folios(mapping, &index, (pgoff_t)-1, &fbatch)) {
		unsigned i;

		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *folio = fbatch.folios[i];
			struct buffer_head *head = folio_buffers(folio);
			struct buffer_head *bh = head;

			if (!head)
				continue;
			do {
				if (buffer_dirty(bh)) {
					write_dirty_buffer(bh, 0);
					wait_on_buffer(bh);
					written++;
				}
				bh = bh->b_this_page;
			} while (bh && bh != head);
			folio_clear_dirty(folio);
		}
		folio_batch_release(&fbatch);
		if (index == (pgoff_t)-1)
			break;
	}
	return written;
}

/* ── the generic address-space operations over buffers ──────────── */

/*
 * Read a whole folio, block by block.
 *
 * A hole reads as zeros rather than as an error: that is what a sparse file
 * is, and a filesystem that returned EIO for one would fail every read of a
 * file it had not written contiguously.
 */
int block_read_full_folio(struct folio *folio, get_block_t *get_block)
{
	struct inode *inode = folio->mapping ? folio->mapping->host : NULL;
	unsigned blocksize;
	sector_t iblock;
	struct buffer_head *head, *bh;
	unsigned long offset = 0;
	int nr = 0;
	int err = 0;

	if (!inode) {
		folio_unlock(folio);
		return -EIO;
	}
	blocksize = 1u << inode->i_blkbits;
	head = folio_create_buffers(folio, blocksize, 0);
	if (!head) {
		folio_unlock(folio);
		return -ENOMEM;
	}
	iblock = (sector_t)((u64)folio->index << PAGE_SHIFT) >> inode->i_blkbits;
	bh = head;
	do {
		if (!buffer_uptodate(bh)) {
			if (!buffer_mapped(bh)) {
				err = get_block(inode, iblock, bh, 0);
				if (err)
					break;
			}
			if (!buffer_mapped(bh)) {
				/* A hole. */
				memset(bh->b_data, 0, blocksize);
				set_buffer_uptodate(bh);
			} else {
				lock_buffer(bh);
				if (!buffer_uptodate(bh)) {
					get_bh(bh);
					bh->b_end_io = end_buffer_read_sync;
					submit_bh(REQ_OP_READ, bh);
					wait_on_buffer(bh);
					if (!buffer_uptodate(bh))
						err = -EIO;
				} else {
					unlock_buffer(bh);
				}
				nr++;
			}
		}
		if (err)
			break;
		iblock++;
		offset += blocksize;
		bh = bh->b_this_page;
	} while (bh && bh != head && offset < PAGE_SIZE);

	if (!err)
		folio_mark_uptodate(folio);
	folio_unlock(folio);
	(void)nr;
	return err;
}

/*
 * Prepare a folio for a write of [pos, pos+len).
 *
 * Every block the write touches is mapped (which is where the filesystem
 * allocates), and a block the write only PARTLY covers is read first — the
 * bytes outside the write have to survive it.
 */
int __block_write_begin_int(struct folio *folio, loff_t pos, unsigned len,
                            get_block_t *get_block, const struct iomap *iomap)
{
	struct inode *inode = folio->mapping ? folio->mapping->host : NULL;
	unsigned blocksize;
	unsigned from = (unsigned)(pos & (PAGE_SIZE - 1));
	unsigned to = from + len;
	unsigned block_start, block_end;
	sector_t block;
	struct buffer_head *head, *bh;
	int err = 0;

	(void)iomap;
	if (!inode)
		return -EIO;
	blocksize = 1u << inode->i_blkbits;
	head = folio_create_buffers(folio, blocksize, 0);
	if (!head)
		return -ENOMEM;
	block = (sector_t)((u64)folio->index << PAGE_SHIFT) >> inode->i_blkbits;

	bh = head;
	for (block_start = 0; bh && block_start < PAGE_SIZE;
	     block++, block_start = block_end, bh = bh->b_this_page) {
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			/* Outside the write. Only its up-to-dateness matters, and only
			 * if the folio as a whole is going to be marked up to date. */
			if (folio_test_uptodate(folio) && !buffer_uptodate(bh))
				set_buffer_uptodate(bh);
			continue;
		}
		if (!buffer_mapped(bh)) {
			if (!get_block)
				return -EIO;
			clear_buffer_new(bh);
			err = get_block(inode, block, bh, 1);
			if (err)
				break;
			if (buffer_new(bh)) {
				/* Freshly allocated: it has no contents on disk, so the
				 * part of it the write does not cover must be zeroed here
				 * rather than read. */
				if (folio_test_uptodate(folio)) {
					clear_buffer_new(bh);
					set_buffer_uptodate(bh);
					mark_buffer_dirty(bh);
					continue;
				}
				if (block_end > to || block_start < from) {
					unsigned zfrom = block_start;
					unsigned zto = block_end;

					if (block_start < from)
						zto = from;
					if (block_end > to)
						zfrom = to;
					if (zto > zfrom)
						memset(bh->b_data + (zfrom - block_start), 0,
						       zto - zfrom);
				}
				continue;
			}
		}
		if (folio_test_uptodate(folio)) {
			set_buffer_uptodate(bh);
			continue;
		}
		if (!buffer_uptodate(bh) && !buffer_delay(bh) &&
		    !buffer_unwritten(bh) &&
		    (block_start < from || block_end > to)) {
			lock_buffer(bh);
			if (!buffer_uptodate(bh)) {
				get_bh(bh);
				bh->b_end_io = end_buffer_read_sync;
				submit_bh(REQ_OP_READ, bh);
				wait_on_buffer(bh);
				if (!buffer_uptodate(bh))
					err = -EIO;
			} else {
				unlock_buffer(bh);
			}
			if (err)
				break;
		}
	}
	return err;
}

int __block_write_begin(struct page *page, loff_t pos, unsigned len,
                        get_block_t *get_block)
{
	return __block_write_begin_int(page_folio(page), pos, len, get_block, NULL);
}

int block_write_begin(struct address_space *mapping, loff_t pos, unsigned len,
                      struct page **pagep, get_block_t *get_block)
{
	pgoff_t index = (pgoff_t)(pos >> PAGE_SHIFT);
	struct folio *folio;
	int err;

	folio = __filemap_get_folio(mapping, index, FGP_WRITEBEGIN,
	                            mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return (int)PTR_ERR(folio);
	err = __block_write_begin_int(folio, pos, len, get_block, NULL);
	if (err) {
		folio_unlock(folio);
		folio_put(folio);
		return err;
	}
	*pagep = folio_page(folio, 0);
	return 0;
}

/*
 * The write has been copied in: record it.
 *
 * Every buffer the write covered becomes up to date and dirty, and the file
 * grows if the write went past its end. `copied` is what actually arrived,
 * which can be less than was asked for.
 */
static int block_write_end_common(struct folio *folio, loff_t pos,
                                  unsigned len, unsigned copied)
{
	struct inode *inode = folio->mapping ? folio->mapping->host : NULL;
	unsigned blocksize;
	unsigned from = (unsigned)(pos & (PAGE_SIZE - 1));
	unsigned to = from + copied;
	unsigned block_start, block_end;
	struct buffer_head *head = folio_buffers(folio);
	struct buffer_head *bh = head;
	int partial = 0;

	(void)len;
	if (!inode || !head)
		return -EIO;
	blocksize = 1u << inode->i_blkbits;
	for (block_start = 0; bh && block_start < PAGE_SIZE;
	     block_start = block_end, bh = bh->b_this_page) {
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			if (!buffer_uptodate(bh))
				partial = 1;
			continue;
		}
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		clear_buffer_new(bh);
	}
	/* The folio is up to date only when every one of its buffers is: a folio
	 * marked uptodate with a stale buffer in it hands out bytes that were
	 * never read. */
	if (!partial)
		folio_mark_uptodate(folio);
	if (pos + copied > i_size_read(inode))
		i_size_write(inode, pos + copied);
	return 0;
}

int block_write_end(struct file *file, struct address_space *mapping,
                    loff_t pos, unsigned len, unsigned copied,
                    struct page *page, void *fsdata)
{
	struct folio *folio = page_folio(page);
	int err;

	(void)file;
	(void)mapping;
	(void)fsdata;
	err = block_write_end_common(folio, pos, len, copied);
	return err ? err : (int)copied;
}

int generic_write_end(struct file *file, struct address_space *mapping,
                      loff_t pos, unsigned len, unsigned copied,
                      struct page *page, void *fsdata)
{
	int ret = block_write_end(file, mapping, pos, len, copied, page, fsdata);
	struct folio *folio = page_folio(page);

	folio_unlock(folio);
	folio_put(folio);
	return ret;
}

int block_commit_write(struct page *page, unsigned from, unsigned to)
{
	struct folio *folio = page_folio(page);
	loff_t pos = ((loff_t)folio->index << PAGE_SHIFT) + from;

	return block_write_end_common(folio, pos, to - from, to - from);
}

/*
 * Write a whole folio back, block by block.
 *
 * Blocks past the end of the file are skipped rather than written: they hold
 * whatever the last partial write left, and putting that on the disk would
 * publish bytes the file does not have.
 */
int block_write_full_page(struct page *page, get_block_t *get_block,
                          struct writeback_control *wbc)
{
	struct folio *folio = page_folio(page);
	struct inode *inode = folio->mapping ? folio->mapping->host : NULL;
	unsigned blocksize;
	sector_t block;
	struct buffer_head *head, *bh;
	loff_t i_size;
	unsigned block_start;
	int err = 0;

	(void)wbc;
	if (!inode) {
		folio_unlock(folio);
		return -EIO;
	}
	blocksize = 1u << inode->i_blkbits;
	head = folio_buffers(folio);
	if (!head) {
		head = folio_create_buffers(folio, blocksize, 0);
		if (!head) {
			folio_unlock(folio);
			return -ENOMEM;
		}
	}
	i_size = i_size_read(inode);
	block = (sector_t)((u64)folio->index << PAGE_SHIFT) >> inode->i_blkbits;

	bh = head;
	folio_start_writeback(folio);
	for (block_start = 0; bh && block_start < PAGE_SIZE;
	     block_start += blocksize, block++, bh = bh->b_this_page) {
		loff_t block_pos = ((loff_t)folio->index << PAGE_SHIFT) + block_start;

		if (block_pos >= i_size)
			break;
		if (!buffer_mapped(bh) && get_block) {
			err = get_block(inode, block, bh, 1);
			if (err)
				break;
		}
		if (!buffer_mapped(bh) || !buffer_dirty(bh))
			continue;
		lock_buffer(bh);
		if (test_clear_buffer_dirty(bh)) {
			get_bh(bh);
			bh->b_end_io = end_buffer_write_sync;
			submit_bh(REQ_OP_WRITE, bh);
			wait_on_buffer(bh);
		} else {
			unlock_buffer(bh);
		}
	}
	folio_end_writeback(folio);
	folio_clear_dirty(folio);
	folio_unlock(folio);
	return err;
}

int block_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf,
                       get_block_t get_block)
{
	(void)vma;
	(void)vmf;
	(void)get_block;
	/* The fault path that makes a shared mapping writable. Nothing maps an
	 * imported filesystem's file yet, and a version that reported success
	 * without preparing the blocks would let a store land on a page with no
	 * allocation behind it. */
	return -EOPNOTSUPP;
}

/*
 * fsync for a filesystem whose metadata lives in buffers: write the data, then
 * the buffers the inode is associated with.
 */
int generic_buffers_fsync_noflush(struct file *file, loff_t start, loff_t end,
                                  bool datasync)
{
	struct inode *inode = file_inode(file);
	int err;

	(void)datasync;
	err = file_write_and_wait_range(file, start, end);
	if (err)
		return err;
	if (inode && inode->i_sb && inode->i_sb->s_bdev)
		lkpi_write_dirty_buffers(inode->i_sb->s_bdev->bd_inode->i_mapping);
	return 0;
}

int generic_buffers_fsync(struct file *file, loff_t start, loff_t end,
                          bool datasync)
{
	int err = generic_buffers_fsync_noflush(file, start, end, datasync);

	if (!err) {
		struct inode *inode = file_inode(file);

		if (inode && inode->i_sb && inode->i_sb->s_bdev)
			sync_blockdev(inode->i_sb->s_bdev);
	}
	return err;
}

struct buffer_head *getblk_unmovable(struct block_device *bdev, sector_t block,
                                     unsigned size)
{
	return __getblk(bdev, block, size);
}

/*
 * Drop the buffers a truncate or an invalidate has made irrelevant.
 *
 * A buffer wholly inside the invalidated range is cleaned and forgotten; one
 * that straddles the edge is kept, because the part outside the range is still
 * the file's.
 */
void block_invalidate_folio(struct folio *folio, size_t offset, size_t length)
{
	struct buffer_head *head = folio_buffers(folio);
	struct buffer_head *bh = head;
	size_t curr = 0;
	size_t stop = length + offset;

	if (!head)
		return;
	do {
		size_t next = curr + bh->b_size;

		if (curr >= offset && next <= stop) {
			clear_buffer_dirty(bh);
			clear_buffer_mapped(bh);
			clear_buffer_uptodate(bh);
			clear_buffer_req(bh);
			clear_buffer_new(bh);
			bh->b_bdev = NULL;
		}
		curr = next;
		bh = bh->b_this_page;
	} while (bh && bh != head);

	/* The whole folio: the ring can go with it, unless somebody still holds a
	 * buffer in it. */
	if (offset == 0 && length >= PAGE_SIZE)
		try_to_free_buffers(folio);
}

/*
 * Write out the buffers an inode has associated with itself.
 *
 * ext3's ordered mode is what this exists for upstream; ext4 calls it from
 * its fsync path. The association is made by mark_buffer_dirty_inode.
 */
int sync_mapping_buffers(struct address_space *mapping)
{
	if (!mapping || !mapping->host || !mapping->host->i_sb ||
	    !mapping->host->i_sb->s_bdev)
		return 0;
	return lkpi_write_dirty_buffers(
	           mapping->host->i_sb->s_bdev->bd_inode->i_mapping) >= 0 ? 0 : -EIO;
}

/*
 * Dirtying a folio that carries buffers dirties the buffers too.
 *
 * ext4 installs this as its `dirty_folio` for the ordered and writeback modes.
 * Marking only the folio would leave writeback with nothing to write: the
 * buffers are what carry the block numbers.
 */
bool block_dirty_folio(struct address_space *mapping, struct folio *folio)
{
	struct buffer_head *head = folio_buffers(folio);
	struct buffer_head *bh = head;

	if (head) {
		do {
			set_buffer_dirty(bh);
			bh = bh->b_this_page;
		} while (bh && bh != head);
	}
	return filemap_dirty_folio(mapping, folio);
}

/*
 * Is the part of the folio the reader wants already up to date?
 *
 * A folio is only "uptodate" as a whole, so a reader of a range inside a
 * partially-filled folio asks this instead — and the answer is per buffer.
 */
bool block_is_partially_uptodate(struct folio *folio, size_t from, size_t count)
{
	struct buffer_head *head = folio_buffers(folio);
	struct buffer_head *bh = head;
	size_t curr = 0;
	size_t to = from + count;

	if (!head)
		return false;
	do {
		size_t next = curr + bh->b_size;

		if (next > from && curr < to && !buffer_uptodate(bh))
			return false;
		curr = next;
		bh = bh->b_this_page;
	} while (bh && bh != head);
	return true;
}

/*
 * Mark a folio as under writeback, reporting whether it already was.
 *
 * `keep_write` is upstream's flag for a redirty that must survive the
 * transition; nothing here dirties a folio behind writeback's back, so the
 * distinction has no effect and the return value is what callers use.
 */
bool __folio_start_writeback(struct folio *folio, bool keep_write)
{
	bool was = folio_test_writeback(folio);

	(void)keep_write;
	if (!was)
		folio_start_writeback(folio);
	return was;
}
