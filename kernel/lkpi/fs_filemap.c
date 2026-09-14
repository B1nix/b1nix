/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the page cache index.
 *
 * An `address_space` is a map from file offset to page, and this file is that
 * map: finding a folio, creating one, removing it, and walking ranges of them.
 * The per-page state — the lock, the dirty and uptodate bits, the copies — is
 * in kernel/lkpi/filemap.c on the other side of the boundary; this is the part
 * that needs the real `struct address_space`, so it is compiled with the
 * imported headers.
 *
 * The index is the mapping's own xarray, keyed by page offset. A page is in the
 * cache if and only if it is in that array, and the array holds a reference to
 * every page in it — so removing an entry is what drops that reference, and a
 * removal that forgets to leaks the page for the life of the boot.
 *
 * What is deliberately absent: reclaim. Nothing here evicts a clean page under
 * memory pressure, because b1nix's reclaim does not know about this cache yet.
 * A filesystem that reads a large file therefore grows the cache until
 * something calls truncate_inode_pages. That is a real limitation and it is the
 * next thing this file needs.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/xarray.h>
#include <linux/slab.h>
#include <linux/writeback.h>
#include <lkpi/page.h>

/* ── the index ──────────────────────────────────────────────────── */

/*
 * Allocate a page for the cache.
 *
 * Zeroed, because a folio handed to a filesystem that then fails to fill it
 * must not expose whatever was in that frame — and "then fails" includes a
 * short read at the end of a file, which is the ordinary case rather than an
 * error.
 */
static struct folio *filemap_alloc_folio_zeroed(void)
{
	struct page *page = lkpi_alloc_page();

	if (!page)
		return NULL;
	memset(page_address(page), 0, PAGE_SIZE);
	page->mapping = NULL;
	page->index = 0;
	page->flags = 0;
	return page_folio(page);
}

struct folio *filemap_alloc_folio(gfp_t gfp, unsigned int order)
{
	(void)gfp;
	/* Order is always zero here: one page per folio. A caller asking for more
	 * gets one page, which every caller handles — they check the folio's own
	 * size rather than assuming the one they asked for. */
	(void)order;
	return filemap_alloc_folio_zeroed();
}

struct page *__page_cache_alloc(gfp_t gfp)
{
	struct folio *folio = filemap_alloc_folio(gfp, 0);

	return folio ? folio_page(folio, 0) : NULL;
}

/*
 * Insert a folio at `index`.
 *
 * The mapping's reference is taken here and dropped by the removal. The folio
 * is published with its mapping and index already set, so that anything which
 * finds it in the array immediately sees where it belongs.
 *
 * Insert-only, under the array's lock: a slot that is already taken is left
 * alone and the caller loses. Storing first and putting the previous entry
 * back afterwards let two racing losers swap each other's folio back in, and
 * the index ended up holding a folio its owner had already unmapped and freed
 * -- every later lookup of that offset saw `mapping` mismatch and retried for
 * ever (four execs of one library, all spinning in __filemap_get_folio).
 */
int filemap_add_folio(struct address_space *mapping, struct folio *folio,
                      pgoff_t index, gfp_t gfp)
{
	int err;

	(void)gfp;
	if (!mapping || !folio)
		return -EINVAL;
	folio->mapping = mapping;
	folio->index = index;
	folio_get(folio);
	xa_lock_irq(&mapping->i_pages);
	err = __xa_insert(&mapping->i_pages, index, folio, GFP_KERNEL);
	if (!err)
		mapping->nrpages++;
	xa_unlock_irq(&mapping->i_pages);
	if (err) {
		folio->mapping = NULL;
		folio_put(folio);
		return err == -EBUSY ? -EEXIST : err;
	}
	return 0;
}

int add_to_page_cache_lru(struct page *page, struct address_space *mapping,
                          pgoff_t index, gfp_t gfp)
{
	return filemap_add_folio(mapping, page_folio(page), index, gfp);
}

void filemap_remove_folio(struct folio *folio)
{
	struct address_space *mapping = folio ? folio->mapping : NULL;

	void *old;

	if (!mapping)
		return;
	/* Only the entry that is this folio: two removals racing on one folio
	 * must drop the mapping's reference once, not twice. */
	xa_lock_irq(&mapping->i_pages);
	old = __xa_cmpxchg(&mapping->i_pages, folio->index, folio, NULL, 0);
	if (old == folio && mapping->nrpages)
		mapping->nrpages--;
	xa_unlock_irq(&mapping->i_pages);
	if (old != folio)
		return;
	folio->mapping = NULL;
	/* The mapping's own reference, released now that nothing can find it. */
	folio_put(folio);
}

void delete_from_page_cache(struct page *page)
{
	filemap_remove_folio(page_folio(page));
}

/* ── finding ────────────────────────────────────────────────────── */

/*
 * The first folio at or after *index and no further than last, with a
 * reference, or NULL; *index is left at the one found.
 *
 * Under the array's lock. A removal erases the entry under that lock and only
 * then drops the mapping's reference, so an entry seen here still holds that
 * reference while this one is taken. Looked up and pinned without the lock,
 * a folio removed in between was freed first and the pin incremented a count
 * inside freed heap memory -- a heap block's header, a btrfs extent buffer's
 * folio with its mapping gone.
 */
static struct folio *filemap_find_get(struct address_space *mapping,
                                      unsigned long *index, unsigned long last)
{
	struct folio *folio;

	xa_lock_irq(&mapping->i_pages);
	folio = xa_find(&mapping->i_pages, index, last, XA_PRESENT);
	if (folio)
		folio_get(folio);
	xa_unlock_irq(&mapping->i_pages);
	return folio;
}

struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index)
{
	unsigned long at = index;

	if (!mapping)
		return NULL;
	return filemap_find_get(mapping, &at, index);
}

struct folio *filemap_lock_folio(struct address_space *mapping, pgoff_t index)
{
	struct folio *folio = filemap_get_folio(mapping, index);

	if (!folio)
		return NULL;
	folio_lock(folio);
	/*
	 * Re-check after the lock: a truncate can have removed the folio from the
	 * mapping while this caller waited, and returning it then hands out a
	 * page the file no longer has.
	 */
	if (folio->mapping != mapping) {
		folio_unlock(folio);
		folio_put(folio);
		return NULL;
	}
	return folio;
}

/*
 * The general form: find, and optionally create, lock, or both.
 *
 * The flags are how a caller says what it wants, and mixing them up is how a
 * path ends up either allocating a page it should have found or using one it
 * never locked. FGP_CREAT without FGP_LOCK returns an unlocked new folio,
 * which is almost never what a filesystem means — but it is what upstream
 * does, so it is what happens here.
 */
struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index,
                                  fgf_t fgp_flags, gfp_t gfp)
{
	struct folio *folio;

	if (!mapping)
		return ERR_PTR(-EINVAL);

	for (;;) {
		folio = filemap_get_folio(mapping, index);
		if (folio) {
			if (fgp_flags & FGP_LOCK) {
				if (fgp_flags & FGP_NOWAIT) {
					if (!folio_trylock(folio)) {
						folio_put(folio);
						return ERR_PTR(-EAGAIN);
					}
				} else {
					folio_lock(folio);
				}
				if (folio->mapping != mapping) {
					/* Truncated under us; start again. */
					folio_unlock(folio);
					folio_put(folio);
					continue;
				}
			}
			if (fgp_flags & FGP_ACCESSED)
				folio_mark_accessed(folio);
			return folio;
		}

		if (!(fgp_flags & FGP_CREAT))
			return ERR_PTR(-ENOENT);

		folio = filemap_alloc_folio(gfp, 0);
		if (!folio)
			return ERR_PTR(-ENOMEM);
		/* Locked before it is published: a folio in the index that nobody has
		 * filled yet must not be usable by a second finder. */
		if (fgp_flags & FGP_LOCK)
			folio_lock(folio);
		if (filemap_add_folio(mapping, folio, index, gfp) == 0)
			return folio;
		/* Somebody else inserted one first. Drop ours and take theirs. */
		if (fgp_flags & FGP_LOCK)
			folio_unlock(folio);
		folio_put(folio);
	}
}

struct folio *filemap_grab_folio(struct address_space *mapping, pgoff_t index)
{
	return __filemap_get_folio(mapping, index, FGP_WRITEBEGIN,
	                           mapping_gfp_mask(mapping));
}

struct page *pagecache_get_page(struct address_space *mapping, pgoff_t index,
                                fgf_t fgp_flags, gfp_t gfp)
{
	struct folio *folio = __filemap_get_folio(mapping, index, fgp_flags, gfp);

	if (IS_ERR(folio))
		return NULL;
	return folio_page(folio, 0);
}

struct page *find_get_page(struct address_space *mapping, pgoff_t offset)
{
	struct folio *folio = filemap_get_folio(mapping, offset);

	return folio ? folio_page(folio, 0) : NULL;
}

struct page *find_get_page_flags(struct address_space *mapping, pgoff_t offset,
                                 fgf_t fgp_flags)
{
	return pagecache_get_page(mapping, offset, fgp_flags, 0);
}

struct page *find_lock_page(struct address_space *mapping, pgoff_t index)
{
	struct folio *folio = filemap_lock_folio(mapping, index);

	return folio ? folio_page(folio, 0) : NULL;
}

struct page *find_or_create_page(struct address_space *mapping, pgoff_t index,
                                 gfp_t gfp_mask)
{
	return pagecache_get_page(mapping, index, FGP_LOCK | FGP_CREAT, gfp_mask);
}

struct page *grab_cache_page_write_begin(struct address_space *mapping,
                                         pgoff_t index)
{
	return pagecache_get_page(mapping, index, FGP_WRITEBEGIN,
	                          mapping_gfp_mask(mapping));
}

/* ── reading through the mapping ────────────────────────────────── */

/*
 * Get a folio and make sure it holds the file's bytes.
 *
 * The distinction from filemap_get_folio is that this one performs the I/O: a
 * caller that used the other and read the bytes would be reading an empty page.
 * The filler is the address_space's own read_folio, which is what knows where
 * the bytes are on disk.
 */
struct folio *read_cache_folio(struct address_space *mapping, pgoff_t index,
                               int (*filler)(struct file *, struct folio *),
                               struct file *file)
{
	struct folio *folio;
	int ret;

	folio = __filemap_get_folio(mapping, index, FGP_LOCK | FGP_CREAT,
	                            mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return folio;

	if (folio_test_uptodate(folio)) {
		folio_unlock(folio);
		return folio;
	}

	if (!filler)
		filler = mapping->a_ops ? mapping->a_ops->read_folio : NULL;
	if (!filler) {
		/* Nothing can fill it. Returning the empty folio would hand the
		 * caller zeros as if they were the file's contents. */
		folio_unlock(folio);
		folio_put(folio);
		return ERR_PTR(-EIO);
	}

	/* read_folio unlocks the folio itself, whether it succeeds or fails —
	 * that is its contract, and unlocking again here would release a lock
	 * somebody else has since taken. */
	ret = filler(file, folio);
	if (ret) {
		folio_put(folio);
		return ERR_PTR(ret);
	}
	/*
	 * Then WAIT for it. A read_folio may be asynchronous: btrfs submits the
	 * bio and returns, and the folio becomes uptodate when the completion
	 * runs on a workqueue. The lock it holds until then is the signal, so
	 * checking uptodate on return reads the answer before it exists — every
	 * read of a file with real extents came back -EIO, while inline files,
	 * whose data is in the item and needs no bio, read fine.
	 */
	folio_wait_locked(folio);
	if (!folio_test_uptodate(folio)) {
		folio_put(folio);
		return ERR_PTR(-EIO);
	}
	return folio;
}

struct folio *read_mapping_folio(struct address_space *mapping, pgoff_t index,
                                 struct file *file)
{
	return read_cache_folio(mapping, index, NULL, file);
}

struct page *read_cache_page(struct address_space *mapping, pgoff_t index,
                             int (*filler)(struct file *, struct folio *),
                             struct file *file)
{
	struct folio *folio = read_cache_folio(mapping, index, filler, file);

	if (IS_ERR(folio))
		return (struct page *)folio;
	return folio_page(folio, 0);
}

struct page *read_cache_page_gfp(struct address_space *mapping, pgoff_t index,
                                 gfp_t gfp)
{
	(void)gfp;
	return read_cache_page(mapping, index, NULL, NULL);
}

struct page *read_mapping_page(struct address_space *mapping, pgoff_t index,
                               struct file *file)
{
	return read_cache_page(mapping, index, NULL, file);
}

size_t memcpy_from_file_folio(char *to, struct folio *folio, loff_t pos,
                              size_t len)
{
	size_t offset = offset_in_folio(folio, pos);
	size_t avail = folio_size(folio) - offset;

	if (len > avail)
		len = avail;
	memcpy_from_folio(to, folio, offset, len);
	return len;
}

/* ── walking ranges ─────────────────────────────────────────────── */

/*
 * Collect the folios in [*start, end] into a batch, advancing *start.
 *
 * The walk is by index rather than through an iterator over the array, which
 * makes it proportional to the RANGE rather than to the number of folios in
 * it. For a sparse file that is slower than upstream and never wrong; for the
 * dense ranges a filesystem actually writes back it is the same.
 */
unsigned filemap_get_folios(struct address_space *mapping, pgoff_t *start,
                            pgoff_t end, struct folio_batch *fbatch)
{
	unsigned long index = *start;
	struct folio *folio;
	unsigned found = 0;

	while (index <= end && found < PAGEVEC_SIZE &&
	       (folio = filemap_find_get(mapping, &index, end)) != NULL) {
		fbatch->folios[fbatch->nr++] = folio;
		found++;
		*start = (pgoff_t)index + 1;
		if (index == end)
			break;
		index++;
	}
	if (!found)
		*start = end == (pgoff_t)-1 ? end : end + 1;
	return found;
}

unsigned filemap_get_folios_contig(struct address_space *mapping,
                                   pgoff_t *start, pgoff_t end,
                                   struct folio_batch *fbatch)
{
	pgoff_t index = *start;
	unsigned found = 0;

	/* Stops at the first gap, which is what "contig" means: the caller is
	 * building one I/O and a hole ends it. */
	while (index <= end && found < PAGEVEC_SIZE) {
		struct folio *folio = filemap_get_folio(mapping, index);

		if (!folio)
			break;
		fbatch->folios[fbatch->nr++] = folio;
		found++;
		index++;
	}
	*start = index;
	return found;
}

unsigned filemap_get_folios_tag(struct address_space *mapping, pgoff_t *start,
                                pgoff_t end, xa_mark_t tag,
                                struct folio_batch *fbatch)
{
	unsigned long index = *start;
	struct folio *folio;
	unsigned found = 0;
	pgoff_t next = 0;

	/*
	 * Over the array's entries, not over the index range: writeback asks for
	 * the whole file, which is `end == (pgoff_t)-1`, and walking that index
	 * by index does not finish. It was 2^64 lookups behind an fsync that
	 * appeared to hang.
	 */
	while (found < PAGEVEC_SIZE &&
	       (folio = filemap_find_get(mapping, &index, end)) != NULL) {
		bool match;

		/*
		 * The tags are not maintained in the array; the state is read from
		 * the folio itself, which gives the same answer for the two tags a
		 * filesystem uses — DIRTY and WRITEBACK are folio flags.
		 */
		if (tag == XA_MARK_1)
			match = folio_test_writeback(folio);
		else
			match = folio_test_dirty(folio);
		if (match) {
			fbatch->folios[fbatch->nr++] = folio;
			found++;
			next = (pgoff_t)index;
		} else {
			folio_put(folio);
		}
		if (index >= end)
			break;
		index++;
	}
	/* Resume after the last one taken; when nothing matched the range is
	 * exhausted, and saying so is what ends the caller's loop. */
	if (found)
		*start = (next == (pgoff_t)-1) ? next : next + 1;
	else
		*start = (end == (pgoff_t)-1) ? end : end + 1;
	return found;
}

bool mapping_tagged(struct address_space *mapping, xa_mark_t tag)
{
	(void)tag;
	/* "Is anything tagged?" — answered as "is anything cached", which is
	 * conservative: a caller that gets true walks the range and finds
	 * nothing, where a false negative would skip writeback entirely. */
	return mapping && mapping->nrpages != 0;
}

/* ── truncate and invalidate ────────────────────────────────────── */

/*
 * Drop every cached folio in a range.
 *
 * Dirty folios are dropped too — that is what makes this a truncate rather
 * than a writeback: the bytes are going away, and writing them out first would
 * be work whose result is immediately discarded.
 */
void truncate_inode_pages_range(struct address_space *mapping, loff_t lstart,
                                loff_t lend)
{
	pgoff_t start = (pgoff_t)(lstart >> PAGE_SHIFT);
	pgoff_t end = (lend == (loff_t)-1) ? (pgoff_t)-1
	                                   : (pgoff_t)(lend >> PAGE_SHIFT);
	unsigned long index = start;
	struct folio *folio;

	if (!mapping)
		return;
	/*
	 * The walk is over the array's ENTRIES, not over the index range.
	 *
	 * Walking indices looks equivalent and is not: a whole-file range ends at
	 * (pgoff_t)-1, so the loop runs 2^64 times over an array holding a
	 * handful of folios. It showed up as a mount that never finished, with
	 * 96% of the kernel's samples inside this function's xa_load.
	 */
	while ((folio = filemap_find_get(mapping, &index, end)) != NULL) {
		folio_lock(folio);
		if (folio->mapping == mapping) {
			folio_clear_dirty(folio);
			filemap_remove_folio(folio);
		}
		folio_unlock(folio);
		folio_put(folio);
		if (index >= end)
			break;
		index++;
	}
}

void truncate_inode_pages(struct address_space *mapping, loff_t lstart)
{
	truncate_inode_pages_range(mapping, lstart, (loff_t)-1);
}

void truncate_inode_pages_final(struct address_space *mapping)
{
	truncate_inode_pages(mapping, 0);
}

void truncate_pagecache(struct inode *inode, loff_t newsize)
{
	truncate_inode_pages(inode->i_mapping, newsize);
}

void truncate_pagecache_range(struct inode *inode, loff_t offset, loff_t end)
{
	truncate_inode_pages_range(inode->i_mapping, offset, end);
}

void truncate_setsize(struct inode *inode, loff_t newsize)
{
	i_size_write(inode, newsize);
	truncate_pagecache(inode, newsize);
}

unsigned long invalidate_mapping_pages(struct address_space *mapping,
                                       pgoff_t start, pgoff_t end)
{
	unsigned long index = start;
	struct folio *folio;
	unsigned long dropped = 0;

	if (!mapping)
		return 0;
	/* Over the entries, for the reason in truncate_inode_pages_range. */
	while ((folio = filemap_find_get(mapping, &index, end)) != NULL) {
		if (folio_trylock(folio)) {
			/* Only CLEAN folios: an invalidate must not lose a write that
			 * has not reached the disk. A dirty one is left alone, which is
			 * why this returns a count rather than succeeding. */
			if (!folio_test_dirty(folio) && !folio_test_writeback(folio) &&
			    folio->mapping == mapping) {
				filemap_remove_folio(folio);
				dropped++;
			}
			folio_unlock(folio);
		}
		folio_put(folio);
		if (index >= end)
			break;
		index++;
	}
	return dropped;
}

int invalidate_inode_pages2_range(struct address_space *mapping, pgoff_t start,
                                  pgoff_t end)
{
	invalidate_mapping_pages(mapping, start, end);
	return 0;
}

int invalidate_inode_pages2(struct address_space *mapping)
{
	return invalidate_inode_pages2_range(mapping, 0, (pgoff_t)-1);
}

/* ── writeback ──────────────────────────────────────────────────── */

/*
 * Write every dirty folio in a range through the mapping's own writepages.
 *
 * The filesystem is what knows where the bytes go, so this is a driver for its
 * operation rather than an implementation of writeback. When a mapping has no
 * writepages, its dirty folios stay dirty — which is correct for a mapping
 * whose owner writes them by another route, and is why this is not an error.
 */
int filemap_fdatawrite_wbc(struct address_space *mapping,
                           struct writeback_control *wbc)
{
	if (!mapping || !mapping->a_ops || !mapping->a_ops->writepages)
		return 0;
	return mapping->a_ops->writepages(mapping, wbc);
}

int filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
                             loff_t end)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = LONG_MAX,
		.range_start = start,
		.range_end = end,
	};
	return filemap_fdatawrite_wbc(mapping, &wbc);
}

int filemap_fdatawrite(struct address_space *mapping)
{
	return filemap_fdatawrite_range(mapping, 0, (loff_t)-1);
}

int filemap_flush(struct address_space *mapping)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_NONE,
		.nr_to_write = LONG_MAX,
		.range_start = 0,
		.range_end = (loff_t)-1,
	};

	return filemap_fdatawrite_wbc(mapping, &wbc);
}

/*
 * Wait for writeback to finish over a range.
 *
 * Completions here are synchronous — see kernel/lkpi/bio.c — so by the time
 * this is called the folios are no longer under writeback and the wait is a
 * check. It is still a loop rather than a no-op, because the day submissions
 * become asynchronous this is the function that has to block, and a stub would
 * have to be found first.
 */
int filemap_fdatawait_range(struct address_space *mapping, loff_t start_byte,
                            loff_t end_byte)
{
	pgoff_t start = (pgoff_t)(start_byte >> PAGE_SHIFT);
	pgoff_t end = (pgoff_t)(end_byte >> PAGE_SHIFT);
	unsigned long index = start;
	struct folio *folio;

	if (!mapping)
		return 0;
	while ((folio = filemap_find_get(mapping, &index, end)) != NULL) {
		folio_wait_writeback(folio);
		folio_put(folio);
		if (index >= end)
			break;
		index++;
	}
	return filemap_check_errors(mapping);
}

int filemap_fdatawait_range_keep_errors(struct address_space *mapping,
                                        loff_t start_byte, loff_t end_byte)
{
	/* Same wait, but the mapping's error is left for the next reader — which
	 * is the difference the name records. */
	pgoff_t start = (pgoff_t)(start_byte >> PAGE_SHIFT);
	pgoff_t end = (pgoff_t)(end_byte >> PAGE_SHIFT);
	unsigned long index = start;
	struct folio *folio;

	if (!mapping)
		return 0;
	while ((folio = filemap_find_get(mapping, &index, end)) != NULL) {
		folio_wait_writeback(folio);
		folio_put(folio);
		if (index >= end)
			break;
		index++;
	}
	return 0;
}

int filemap_write_and_wait_range(struct address_space *mapping, loff_t lstart,
                                 loff_t lend)
{
	int err = filemap_fdatawrite_range(mapping, lstart, lend);
	int err2 = filemap_fdatawait_range(mapping, lstart, lend);

	/* The FIRST error is the one reported: a write failure followed by a
	 * successful wait must not read as success. */
	return err ? err : err2;
}

int filemap_write_and_wait(struct address_space *mapping)
{
	return filemap_write_and_wait_range(mapping, 0, (loff_t)-1);
}

int filemap_check_errors(struct address_space *mapping)
{
	int ret = 0;

	if (!mapping)
		return 0;
	if (test_and_clear_bit(AS_ENOSPC, &mapping->flags))
		ret = -ENOSPC;
	if (test_and_clear_bit(AS_EIO, &mapping->flags))
		ret = -EIO;
	return ret;
}

int filemap_check_wb_err(struct address_space *mapping, errseq_t since)
{
	return errseq_check(&mapping->wb_err, since);
}

int file_check_and_advance_wb_err(struct file *file)
{
	struct address_space *mapping = file->f_mapping;

	if (!mapping)
		return 0;
	/* Consuming, not peeking: this is the fsync path, and the error is
	 * reported to THIS opener exactly once. */
	return errseq_check_and_advance(&mapping->wb_err, &file->f_wb_err);
}

int file_write_and_wait_range(struct file *file, loff_t lstart, loff_t lend)
{
	int err = filemap_write_and_wait_range(file->f_mapping, lstart, lend);
	int err2 = file_check_and_advance_wb_err(file);

	return err ? err : err2;
}

bool filemap_range_has_page(struct address_space *mapping, loff_t start,
                            loff_t end)
{
	pgoff_t first = (pgoff_t)(start >> PAGE_SHIFT);
	pgoff_t last = (pgoff_t)(end >> PAGE_SHIFT);
	unsigned long index = first;
	struct folio *folio;

	if (!mapping)
		return false;
	folio = filemap_find_get(mapping, &index, last);
	if (!folio)
		return false;
	folio_put(folio);
	return true;
}

bool filemap_range_needs_writeback(struct address_space *mapping, loff_t start,
                                   loff_t end)
{
	pgoff_t first = (pgoff_t)(start >> PAGE_SHIFT);
	pgoff_t last = (pgoff_t)(end >> PAGE_SHIFT);
	unsigned long index = first;
	struct folio *folio;

	if (!mapping)
		return false;
	while ((folio = filemap_find_get(mapping, &index, last)) != NULL) {
		bool busy = folio_test_dirty(folio) || folio_test_writeback(folio);

		folio_put(folio);
		if (busy)
			return true;
		if (index >= last)
			break;
		index++;
	}
	return false;
}

/*
 * Mark a folio dirty in its mapping.
 *
 * Returns whether it was previously clean, which is what tells the caller to
 * account a newly dirty page. Reporting true unconditionally would make the
 * dirty-page count climb without bound.
 */
bool filemap_dirty_folio(struct address_space *mapping, struct folio *folio)
{
	(void)mapping;
	return !test_and_set_bit(PG_dirty, (unsigned long *)&folio->flags);
}

/*
 * The b1nix-side folio_mark_dirty() reaches the mapping's own dirty_folio
 * through this; see the note beside it in kernel/lkpi/filemap.c. Returns
 * whether an operation ran.
 */
int lkpi_dirty_folio_ops(struct folio *folio)
{
	struct address_space *mapping = folio ? folio->mapping : NULL;

	if (!mapping || !mapping->a_ops || !mapping->a_ops->dirty_folio)
		return 0;
	mapping->a_ops->dirty_folio(mapping, folio);
	return 1;
}

bool noop_dirty_folio(struct address_space *mapping, struct folio *folio)
{
	return filemap_dirty_folio(mapping, folio);
}

void folio_cancel_dirty(struct folio *folio)
{
	folio_clear_dirty(folio);
}

void folio_redirty_for_writepage(struct writeback_control *wbc,
                                 struct folio *folio)
{
	/* Put it back on the dirty list: writeback could not deal with it now,
	 * and the pages it did not write must be attempted again — so the budget
	 * is returned as well as the bit. */
	if (wbc)
		wbc->pages_skipped++;
	folio_set_dirty(folio);
}

int redirty_page_for_writepage(struct writeback_control *wbc, struct page *page)
{
	folio_redirty_for_writepage(wbc, page_folio(page));
	return 0;
}

void folio_invalidate(struct folio *folio, size_t offset, size_t length)
{
	struct address_space *mapping = folio->mapping;

	if (mapping && mapping->a_ops && mapping->a_ops->invalidate_folio)
		mapping->a_ops->invalidate_folio(folio, offset, length);
}

bool filemap_release_folio(struct folio *folio, gfp_t gfp)
{
	struct address_space *mapping = folio->mapping;

	if (folio_test_dirty(folio) || folio_test_writeback(folio))
		return false;
	if (mapping && mapping->a_ops && mapping->a_ops->release_folio)
		return mapping->a_ops->release_folio(folio, gfp);
	return true;
}

int generic_error_remove_folio(struct address_space *mapping, struct folio *folio)
{
	if (!mapping)
		return -EINVAL;
	filemap_remove_folio(folio);
	return 0;
}

int filemap_invalidate_inode(struct inode *inode, bool flush, loff_t start,
                             loff_t end)
{
	struct address_space *mapping = inode->i_mapping;

	if (!mapping || !mapping->nrpages || end < start)
		goto out;
	/* Hold off new folios while the range is emptied. */
	filemap_invalidate_lock(mapping);
	if (mapping->nrpages) {
		if (flush)
			filemap_fdatawrite_range(mapping, start, end);
		invalidate_inode_pages2_range(mapping, start / PAGE_SIZE,
		                              end / PAGE_SIZE);
	}
	filemap_invalidate_unlock(mapping);
out:
	return filemap_check_errors(mapping);
}

/*
 * The mapping's invalidate lock.
 *
 * It excludes page-cache population from truncation and hole punching: a read
 * that is filling a folio must not race a truncate that is removing it. Held
 * shared by readers and exclusive by the operations that change the file's
 * extent map.
 */
void filemap_invalidate_lock(struct address_space *mapping)
{ down_write(&mapping->invalidate_lock); }
void filemap_invalidate_unlock(struct address_space *mapping)
{ up_write(&mapping->invalidate_lock); }
void filemap_invalidate_lock_shared(struct address_space *mapping)
{ down_read(&mapping->invalidate_lock); }
void filemap_invalidate_unlock_shared(struct address_space *mapping)
{ up_read(&mapping->invalidate_lock); }

/* ── odds and ends on a folio ───────────────────────────────────── */

bool folio_mapped(struct folio *folio)
{
	/* No folio here is mapped into a process: the mmap path does not go
	 * through this cache yet. A false answer is what lets a truncate proceed
	 * without unmapping, which is correct while that stays true. */
	(void)folio;
	return false;
}

bool folio_maybe_dma_pinned(struct folio *folio)
{
	(void)folio;
	return false;
}

bool folio_test_large(struct folio *folio)
{
	(void)folio;
	return false;
}

void folio_wait_stable(struct folio *folio)
{
	/* Stable pages: wait until writeback has finished reading the folio's
	 * bytes, so a writer does not change them mid-flight. */
	folio_wait_writeback(folio);
}

/*
 * How much of this folio is still inside the file.
 *
 * Zero means the folio is entirely past EOF — a fault on a file that shrank
 * under the mapping has to see that rather than write into a page nothing owns.
 */
size_t folio_mkwrite_check_truncate(struct folio *folio, struct inode *inode)
{
	loff_t size = i_size_read(inode);
	pgoff_t index = size >> PAGE_SHIFT;
	size_t offset = offset_in_folio(folio, size);

	if (!folio->mapping)
		return 0;
	if (folio->index < index)
		return folio_size(folio);
	if (folio->index > index)
		return 0;
	if (!offset)
		return 0;
	return offset;
}

void flush_dcache_page(struct page *page)
{
	/* x86 and arm64 both have physically-indexed data caches, so a page seen
	 * through two virtual addresses needs no flush between them. The call
	 * stays because it marks where an architecture with a virtually-indexed
	 * cache would need one. */
	(void)page;
}

void flush_dcache_folio(struct folio *folio)
{
	(void)folio;
}
