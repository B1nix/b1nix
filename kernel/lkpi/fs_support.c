/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the remainder of what a filesystem calls.
 *
 * Read-ahead, mount-option matching, the kobject and sysfs surface, kernel
 * threads, and the handful of helpers that belong to no larger subsystem. Each
 * is small; what they have in common is that they need the real Linux
 * structures, so they are compiled with the imported headers.
 *
 * Two of them are gaps rather than implementations, and say so where they are:
 * RAID-6 parity, and btrfs's zoned completion. Both fail loudly rather than
 * quietly doing nothing.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/writeback.h>
#include <linux/seq_file.h>
#include <linux/miscdevice.h>
#include <linux/fileattr.h>
#include <linux/exportfs.h>
#include <linux/fiemap.h>
#include <linux/raid/pq.h>
#include <linux/raid/xor.h>
#include <linux/uio.h>
#include <lkpi/env.h>

/* ── read-ahead ─────────────────────────────────────────────────── */

/*
 * Read-ahead, as upstream does it: the missing folios of a range are allocated,
 * locked and put in the cache, and the filesystem's `readahead` operation reads
 * them together -- btrfs builds one bio for the run and verifies the whole
 * batch in one pass of its end-io worker.
 *
 * Without it every page of a buffered read went through read_folio on its own:
 * one bio, one trip through btrfs's end-io workqueue, one wait on the page lock
 * per 4 KiB. A desktop start-up demand-paging its libraries off a btrfs root
 * took 46 000 such waits. b1nix's own page cache already asks for clusters of
 * 16-64 pages at a time; this is what lets such a cluster reach the device as
 * one read.
 *
 * The readahead_control carries the batch still to be handed out: `_index` is
 * the next folio's index and `_nr_pages` how many remain. readahead_folio()
 * gives one back locked, having dropped the reference allocation took -- the
 * cache's own reference keeps it -- and the filesystem unlocks it when its I/O
 * completes. A failure is not an error here: the folio stays not up to date,
 * and read_folio on the demand path tries again.
 */
#define LKPI_RA_MAX_PAGES 64u

struct folio *readahead_folio(struct readahead_control *rac)
{
	struct folio *folio;

	if (!rac || !rac->_nr_pages)
		return NULL;
	folio = xa_load(&rac->mapping->i_pages, rac->_index);
	rac->_index++;
	rac->_nr_pages--;
	if (!folio)
		return NULL;
	folio_put(folio);
	return folio;
}

struct page *readahead_page(struct readahead_control *rac)
{
	struct folio *folio = readahead_folio(rac);

	return folio ? folio_page(folio, 0) : NULL;
}

unsigned readahead_page_batch(struct readahead_control *rac,
                              struct page **array)
{
	(void)rac; (void)array;
	return 0;
}

size_t readahead_batch_length(struct readahead_control *rac)
{
	(void)rac;
	return 0;
}

/* Hand the gathered run to the filesystem, then release whatever it did not
 * take: those folios are unlocked and left not up to date, and their
 * allocation reference dropped. */
static void lkpi_read_pages(struct readahead_control *rac)
{
	const struct address_space_operations *aops = rac->mapping->a_ops;
	struct folio *folio;

	if (!rac->_nr_pages)
		return;
	if (aops && aops->readahead) {
		aops->readahead(rac);
		while ((folio = readahead_folio(rac)) != NULL)
			folio_unlock(folio);
	} else if (aops && aops->read_folio) {
		while ((folio = readahead_folio(rac)) != NULL)
			aops->read_folio(rac->file, folio);
	} else {
		while ((folio = readahead_folio(rac)) != NULL)
			folio_unlock(folio);
	}
}

void page_cache_ra_unbounded(struct readahead_control *rac,
                             unsigned long nr_to_read, unsigned long lookahead)
{
	struct address_space *mapping = rac->mapping;
	struct inode *inode = mapping->host;
	pgoff_t index = rac->_index;
	pgoff_t end_index;
	loff_t isize = i_size_read(inode);

	(void)lookahead;
	if (isize <= 0)
		return;
	end_index = (pgoff_t)((isize - 1) >> PAGE_SHIFT);
	if (nr_to_read > LKPI_RA_MAX_PAGES)
		nr_to_read = LKPI_RA_MAX_PAGES;
	rac->_nr_pages = 0;
	for (unsigned long i = 0; i < nr_to_read; i++) {
		pgoff_t at = index + i;
		struct folio *folio;

		if (at > end_index)
			break;
		if (xa_load(&mapping->i_pages, at)) {
			/* Already cached: the run so far goes out, a new one starts
			 * after this folio. */
			lkpi_read_pages(rac);
			rac->_index = at + 1;
			continue;
		}
		folio = filemap_alloc_folio(mapping_gfp_mask(mapping), 0);
		if (!folio)
			break;
		folio_lock(folio);
		if (filemap_add_folio(mapping, folio, at, mapping_gfp_mask(mapping))) {
			folio_unlock(folio);
			folio_put(folio);
			lkpi_read_pages(rac);
			rac->_index = at + 1;
			continue;
		}
		if (!rac->_nr_pages)
			rac->_index = at;
		rac->_nr_pages++;
	}
	lkpi_read_pages(rac);
}

void page_cache_sync_readahead(struct address_space *mapping,
                               struct file_ra_state *ra, struct file *file,
                               pgoff_t index, unsigned long req_count)
{
	DEFINE_READAHEAD(rac, file, ra, mapping, index);

	if (ra) {
		ra->start = index;
		ra->size = (unsigned int)req_count;
	}
	page_cache_ra_unbounded(&rac, req_count, 0);
}

void page_cache_async_readahead(struct address_space *mapping,
                                struct file_ra_state *ra, struct file *file,
                                struct folio *folio, unsigned long req_count)
{
	page_cache_sync_readahead(mapping, ra, file, folio_next_index(folio),
	                          req_count);
}

/* ── mount options, the old parser ──────────────────────────────── */

/*
 * Match one option against a table of patterns.
 *
 * The substrings point into the CALLER's string, which is why the option text
 * must outlive them — and why match_strdup exists for the cases where it does
 * not.
 */
static int match_one(char *s, const char *p, substring_t args[])
{
	char *meta;
	int argc = 0;

	if (!p)
		return 0;

	for (;;) {
		size_t len;

		meta = strchr(p, '%');
		if (!meta)
			/* No more captures: the rest must match exactly, including the
			 * terminator — a prefix match would accept "commit" for
			 * "commit=". */
			return strcmp(p, s) == 0;

		len = (size_t)(meta - p);
		if (strncmp(p, s, len))
			return 0;
		s += len;
		p = meta + 1;

		if (argc >= MAX_OPT_ARGS)
			return 0;

		args[argc].from = s;
		switch (*p++) {
		case 'd':
		case 'u':
			/* A number: everything up to the first character that cannot be
			 * part of one. */
			while (*s == '-' || (*s >= '0' && *s <= '9'))
				s++;
			break;
		case 'o':
			while (*s >= '0' && *s <= '7')
				s++;
			break;
		case 'x':
			while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
			       (*s >= 'A' && *s <= 'F'))
				s++;
			break;
		case 's': {
			/* A string runs to the end of the option, or to the next literal
			 * character of the pattern. */
			size_t str_len = strlen(s);

			if (str_len == 0)
				return 0;
			s += str_len;
			break;
		}
		default:
			return 0;
		}
		args[argc].to = s;
		argc++;
		if (args[argc - 1].from == args[argc - 1].to)
			return 0;   /* an empty capture is not a match */
	}
}

int match_token(char *s, const match_table_t table, substring_t args[])
{
	const struct { int token; const char *pattern; } *p;

	for (p = (const void *)table; p->pattern; p++)
		if (match_one(s, p->pattern, args))
			break;
	/* The table's last entry is the "no match" token, and reaching it is how a
	 * filesystem reports an unrecognised option. */
	return p->token;
}

static int match_number(substring_t *s, int *result, int base)
{
	char buf[32];
	size_t len = (size_t)(s->to - s->from);
	long value;
	int ret;

	if (len >= sizeof(buf))
		return -ERANGE;
	memcpy(buf, s->from, len);
	buf[len] = '\0';
	ret = kstrtol(buf, (unsigned int)base, &value);
	if (ret)
		return ret;
	*result = (int)value;
	return 0;
}

int match_int(substring_t *s, int *result)
{
	return match_number(s, result, 0);
}

int match_uint(substring_t *s, unsigned int *result)
{
	int v;
	int ret = match_number(s, &v, 0);

	if (ret)
		return ret;
	*result = (unsigned int)v;
	return 0;
}

int match_u64(substring_t *s, u64 *result)
{
	char buf[32];
	size_t len = (size_t)(s->to - s->from);
	u64 value;
	int ret;

	if (len >= sizeof(buf))
		return -ERANGE;
	memcpy(buf, s->from, len);
	buf[len] = '\0';
	ret = kstrtou64(buf, 0, &value);
	if (ret)
		return ret;
	*result = value;
	return 0;
}

int match_octal(substring_t *s, int *result)
{
	return match_number(s, result, 8);
}

int match_hex(substring_t *s, int *result)
{
	return match_number(s, result, 16);
}

size_t match_strlcpy(char *dest, const substring_t *src, size_t size)
{
	size_t len = (size_t)(src->to - src->from);

	if (size) {
		size_t copy = (len >= size) ? size - 1 : len;

		memcpy(dest, src->from, copy);
		dest[copy] = '\0';
	}
	return len;
}

char *match_strdup(const substring_t *s)
{
	size_t len = (size_t)(s->to - s->from);
	char *p = kmalloc(len + 1, GFP_KERNEL);

	if (p) {
		memcpy(p, s->from, len);
		p[len] = '\0';
	}
	return p;
}

/* ── kobjects and sysfs ─────────────────────────────────────────── */

void kobject_del(struct kobject *kobj)
{
	if (kobj)
		kobj->state_in_sysfs = 0;
}

const char *kobject_name(const struct kobject *kobj)
{
	return kobj ? kobj->name : NULL;
}

int kobject_rename(struct kobject *kobj, const char *new_name)
{
	if (!kobj)
		return -EINVAL;
	kobj->name = new_name;
	return 0;
}

struct kset *kset_create_and_add(const char *name,
                                 const struct kset_uevent_ops *uevent_ops,
                                 struct kobject *parent_kobj)
{
	struct kset *kset = kzalloc(sizeof(*kset), GFP_KERNEL);

	if (!kset)
		return NULL;
	INIT_LIST_HEAD(&kset->list);
	kset->uevent_ops = uevent_ops;
	kset->kobj.name = name;
	kset->kobj.parent = parent_kobj;
	kset->kobj.state_initialized = 1;
	return kset;
}

void kset_unregister(struct kset *kset)
{
	kfree(kset);
}

void sysfs_notify(struct kobject *kobj, const char *dir, const char *attr)
{
	(void)kobj; (void)dir; (void)attr;
	/* Wakes a poll on a sysfs file. b1nix's sysfs is generated per read rather
	 * than backed by files that can be polled, so there is nobody to wake. */
}

int sysfs_update_group(struct kobject *kobj, const struct attribute_group *grp)
{
	(void)kobj; (void)grp;
	return 0;
}

void sysfs_remove_files(struct kobject *kobj, const struct attribute * const *ptr)
{
	(void)kobj; (void)ptr;
}

int shrinker_debugfs_rename(struct shrinker *shrinker, const char *fmt, ...)
{
	(void)shrinker; (void)fmt;
	return 0;
}

/* ── misc devices ───────────────────────────────────────────────── */

/*
 * /dev/btrfs-control, which is how `btrfs device scan` tells the kernel about
 * devices before a mount.
 *
 * Registration records the device without publishing a node: b1nix's devfs is
 * populated from its own registry, and the bridge is what will hand it this
 * one. Refusing here would make btrfs fail to load; publishing a node nothing
 * can open would be worse.
 */
static struct miscdevice *misc_registered;

int misc_register(struct miscdevice *misc)
{
	if (!misc)
		return -EINVAL;
	misc->list.next = NULL;
	misc->list.prev = NULL;
	misc_registered = misc;
	return 0;
}

void misc_deregister(struct miscdevice *misc)
{
	if (misc_registered == misc)
		misc_registered = NULL;
}

/*
 * The registered misc device, for the bridge.
 *
 * b1nix's devfs needs to find it to create /dev/btrfs-control, and this is the
 * only place that knows it exists.
 */
struct miscdevice *lkpi_misc_device(void)
{
	return misc_registered;
}

/* ── kernel threads ─────────────────────────────────────────────── */

/*
 * btrfs runs its transaction committer and its cleaner as kthreads, and both
 * are structural: without the committer nothing is ever written, and without
 * the cleaner deleted subvolumes are never reclaimed.
 *
 * Both are stoppable: kthread_stop() raises the thread's stop flag, wakes it
 * and waits until its function has returned, because close_ctree frees the
 * structures the thread is still reading.
 */
struct lkpi_task *kthread_create_on_node(int (*threadfn)(void *data),
                                         void *data, int node,
                                         const char namefmt[], ...)
{
	char name[16]; /* TASK_COMM_LEN */
	va_list ap;

	(void)node;
	/*
	 * `namefmt` is a format and callers pass arguments for it: jbd2 names its
	 * thread "jbd2/%s" after the device. Handed on unformatted, that was the
	 * name /proc and ps showed. The scheduler copies the string.
	 *
	 * b1nix's kthread entry returns void and Linux's returns int, so the
	 * adapter is on the other side of the boundary (kernel/lkpi/fs_misc.c).
	 */
	va_start(ap, namefmt);
	vsnprintf(name, sizeof(name), namefmt, ap);
	va_end(ap);
	return lkpi_fs_kthread_run(threadfn, data, name);
}

/* The b1nix side owns the stop flag and the join; see kernel/lkpi/fs_misc.c. */
int lkpi_kthread_stop(unsigned long handle);

int kthread_stop(struct lkpi_task *k)
{
	return lkpi_kthread_stop((unsigned long)k);
}

void kthread_park(struct lkpi_task *k) { (void)k; }
void kthread_unpark(struct lkpi_task *k) { (void)k; }
bool kthread_should_park(void) { return false; }
void kthread_parkme(void) { }

/* ── bio helpers that need the folio type ───────────────────────── */

/*
 * Folio iteration over a bio's vector.
 *
 * `fi.folio` is NULL when the walk is done, which is what ends
 * bio_for_each_folio_all — so the terminator is set by advancing past the last
 * entry rather than by a separate test.
 */
void bio_first_folio(struct folio_iter *fi, struct bio *bio, int i)
{
	struct bio_vec *bv;

	if (i >= bio->bi_vcnt) {
		fi->folio = NULL;
		return;
	}
	bv = &bio->bi_io_vec[i];
	fi->folio = page_folio(bv->bv_page);
	fi->offset = bv->bv_offset;
	fi->length = bv->bv_len;
	fi->_seg_count = bv->bv_len;
	fi->_i = i;
}

void bio_next_folio(struct folio_iter *fi, struct bio *bio)
{
	bio_first_folio(fi, bio, fi->_i + 1);
}

void *bvec_kmap_local(struct bio_vec *bvec)
{
	return (char *)page_address(bvec->bv_page) + bvec->bv_offset;
}

void memzero_bvec(struct bio_vec *bvec)
{
	memset(bvec_kmap_local(bvec), 0, bvec->bv_len);
}

/*
 * Pin the pages an iterator describes into a bio.
 *
 * Only a bvec iterator can be handled: its pages are already pages, and adding
 * them is a copy of the descriptions. A user-backed iterator would need its
 * pages pinned, which needs the mm side of the bridge — so it is refused rather
 * than half-done, and the caller falls back to the buffered path.
 */
int bio_iov_iter_get_pages(struct bio *bio, struct iov_iter *iter,
                           unsigned len_align_mask)
{
	(void)len_align_mask;
	if (!iov_iter_is_bvec(iter))
		return -EINVAL;

	while (iov_iter_count(iter)) {
		const struct bio_vec *bv = iter->bvec;
		unsigned int len = bv->bv_len;

		if (len > iov_iter_count(iter))
			len = (unsigned int)iov_iter_count(iter);
		if (!bio_add_page(bio, bv->bv_page, len, bv->bv_offset))
			break;
		iov_iter_advance(iter, len);
	}
	return bio->bi_iter.bi_size ? 0 : -EINVAL;
}

int bio_iov_vecs_to_alloc(struct iov_iter *iter, int max_segs)
{
	int n = iov_iter_npages(iter, max_segs);

	return n > 0 ? n : 1;
}

/* ── folios and pages ───────────────────────────────────────────── */

void folio_get(struct folio *folio)
{
	get_page(folio_page(folio, 0));
}

/* virt_to_page is implemented in kernel/lkpi/page.c, next to the registry that
 * answers it. */

struct folio *virt_to_folio(const void *addr)
{
	struct page *page = virt_to_page(addr);

	return page ? page_folio(page) : NULL;
}

unsigned long alloc_pages_bulk_array(gfp_t gfp, unsigned long nr_pages,
                                     struct page **page_array)
{
	unsigned long i;

	(void)gfp;
	/*
	 * Returns how many it managed, which may be fewer than asked. Every
	 * caller loops on that; one that treated a short return as failure would
	 * throw away the pages it was given.
	 */
	for (i = 0; i < nr_pages; i++) {
		if (page_array[i])
			continue;   /* the caller may have filled some already */
		page_array[i] = lkpi_alloc_page();
		if (!page_array[i])
			break;
	}
	return i;
}

size_t kmalloc_size_roundup(size_t size)
{
	/* The heap does not round up, so a caller may use exactly what it asked
	 * for. Reporting more would let it write past its own allocation. */
	return size;
}

/* ── writeback drivers ──────────────────────────────────────────── */

unsigned int wbc_to_write_flags(struct writeback_control *wbc)
{
	unsigned int flags = 0;

	if (wbc->sync_mode == WB_SYNC_ALL)
		flags |= REQ_SYNC;
	else if (wbc->for_kupdate || wbc->for_background)
		flags |= REQ_BACKGROUND;
	return flags;
}

/*
 * Walk a mapping's dirty folios and hand each to the caller's writepage.
 *
 * The budget in `wbc->nr_to_write` is honoured, and a folio the caller could
 * not write is left dirty — those two together are what keep one inode from
 * starving the rest and what stops a failed write being forgotten.
 */
int write_cache_pages(struct address_space *mapping,
                      struct writeback_control *wbc,
                      int (*writepage)(struct folio *,
                                       struct writeback_control *, void *),
                      void *data)
{
	pgoff_t index = wbc->range_cyclic
	                    ? mapping->writeback_index
	                    : (pgoff_t)(wbc->range_start >> PAGE_SHIFT);
	pgoff_t end = (wbc->range_end == (loff_t)-1 || wbc->range_cyclic)
	                  ? (pgoff_t)-1
	                  : (pgoff_t)(wbc->range_end >> PAGE_SHIFT);
	int ret = 0;

	while (index <= end && wbc->nr_to_write > 0) {
		struct folio *folio = filemap_get_folio(mapping, index);

		if (folio) {
			folio_lock(folio);
			if (folio->mapping == mapping && folio_test_dirty(folio)) {
				folio_clear_dirty_for_io(folio);
				ret = writepage(folio, wbc, data);
				if (ret) {
					folio_unlock(folio);
					folio_put(folio);
					break;
				}
				wbc->nr_to_write--;
			} else {
				folio_unlock(folio);
			}
			folio_put(folio);
		}
		if (index == (pgoff_t)-1)
			break;
		index++;
	}
	if (wbc->range_cyclic)
		mapping->writeback_index = index;
	return ret;
}

/*
 * The ->writepages iterator, Linux's own (mm/page-writeback.c) over this page
 * cache's tagged lookup. The folio handed out is locked and has had its dirty
 * bit cleared for I/O; the caller writes it and passes it back in.
 */
static xa_mark_t wbc_to_tag(struct writeback_control *wbc)
{
	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		return PAGECACHE_TAG_TOWRITE;
	return PAGECACHE_TAG_DIRTY;
}

static pgoff_t wbc_end(struct writeback_control *wbc)
{
	if (wbc->range_cyclic)
		return (pgoff_t)-1;
	return (pgoff_t)(wbc->range_end >> PAGE_SHIFT);
}

static bool folio_prepare_writeback(struct address_space *mapping,
                                    struct writeback_control *wbc,
                                    struct folio *folio)
{
	/* Truncated or invalidated under us: nothing to write. */
	if (unlikely(folio->mapping != mapping))
		return false;
	/* Somebody else wrote it. */
	if (!folio_test_dirty(folio))
		return false;
	if (folio_test_writeback(folio)) {
		if (wbc->sync_mode == WB_SYNC_NONE)
			return false;
		folio_wait_writeback(folio);
	}
	BUG_ON(folio_test_writeback(folio));
	return folio_clear_dirty_for_io(folio);
}

static struct folio *writeback_get_folio(struct address_space *mapping,
                                         struct writeback_control *wbc)
{
	struct folio *folio;

retry:
	folio = folio_batch_next(&wbc->fbatch);
	if (!folio) {
		folio_batch_release(&wbc->fbatch);
		cond_resched();
		filemap_get_folios_tag(mapping, &wbc->index, wbc_end(wbc),
		                       wbc_to_tag(wbc), &wbc->fbatch);
		folio = folio_batch_next(&wbc->fbatch);
		if (!folio)
			return NULL;
	}

	folio_lock(folio);
	if (unlikely(!folio_prepare_writeback(mapping, wbc, folio))) {
		folio_unlock(folio);
		goto retry;
	}
	return folio;
}

struct folio *writeback_iter(struct address_space *mapping,
                             struct writeback_control *wbc, struct folio *folio,
                             int *error)
{
	if (!folio) {
		folio_batch_init(&wbc->fbatch);
		wbc->saved_err = *error = 0;
		if (wbc->range_cyclic)
			wbc->index = mapping->writeback_index;
		else
			wbc->index = (pgoff_t)(wbc->range_start >> PAGE_SHIFT);
		if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
			tag_pages_for_writeback(mapping, wbc->index, wbc_end(wbc));
	} else {
		wbc->nr_to_write -= (long)folio_nr_pages(folio);

		WARN_ON_ONCE(*error > 0);
		/* An integrity pass writes everything it tagged and reports the
		 * first error at the end; background writeback stops at the first
		 * error or when the budget runs out. */
		if (wbc->sync_mode == WB_SYNC_ALL) {
			if (*error && !wbc->saved_err)
				wbc->saved_err = *error;
		} else {
			if (*error || wbc->nr_to_write <= 0)
				goto done;
		}
	}

	folio = writeback_get_folio(mapping, wbc);
	if (!folio) {
		if (wbc->range_cyclic)
			mapping->writeback_index = 0;
		*error = wbc->saved_err;
	}
	return folio;

done:
	if (wbc->range_cyclic)
		mapping->writeback_index = folio_next_index(folio);
	folio_batch_release(&wbc->fbatch);
	return NULL;
}

void tag_pages_for_writeback(struct address_space *mapping, pgoff_t start,
                             pgoff_t end)
{
	(void)mapping; (void)start; (void)end;
	/* The TOWRITE tag exists so a second writeback pass does not pick up
	 * folios dirtied after the first began. The tags are not maintained in
	 * the array here (see filemap_get_folios_tag), so a pass may write a
	 * folio dirtied during it — extra work, never lost data. */
}

void balance_dirty_pages_ratelimited(struct address_space *mapping)
{
	(void)mapping;
	/*
	 * The throttle that makes a writer wait when too much is dirty. b1nix's
	 * page cache has its own writeback pressure and this cache is not part of
	 * it, so a writer here is not throttled — which is the reason the cache
	 * can grow without bound, recorded in kernel/lkpi/fs_filemap.c.
	 */
}

int balance_dirty_pages_ratelimited_flags(struct address_space *mapping,
                                          unsigned int flags)
{
	(void)mapping; (void)flags;
	return 0;
}

void inode_attach_wb(struct inode *inode, struct folio *folio)
{
	(void)inode; (void)folio;
}

void __inode_attach_wb(struct inode *inode, struct folio *folio)
{
	(void)inode; (void)folio;
}

void wakeup_flusher_threads(enum wb_reason reason)
{
	(void)reason;
}

int sb_init_dio_done_wq(struct super_block *sb)
{
	if (sb->s_dio_done_wq)
		return 0;
	sb->s_dio_done_wq = alloc_workqueue("dio/%s", WQ_MEM_RECLAIM, 1,
	                                    sb->s_id);
	return sb->s_dio_done_wq ? 0 : -ENOMEM;
}

/* ── mappings ───────────────────────────────────────────────────── */

loff_t mapping_seek_hole_data(struct address_space *mapping, loff_t start,
                              loff_t end, int whence)
{
	(void)mapping;
	/* Without an extent map, everything inside the file is data and the only
	 * hole is at the end. A legal answer — SEEK_DATA may report less
	 * sparseness than a file has — where claiming a hole would not be. */
	if (whence == SEEK_DATA)
		return start < end ? start : -ENXIO;
	return end;
}

bool mapping_writably_mapped(struct address_space *mapping)
{
	return mapping && atomic_read(&mapping->i_mmap_writable) > 0;
}

void pagecache_isize_extended(struct inode *inode, loff_t from, loff_t to)
{
	/*
	 * The file grew past the end of a block that already existed. The bytes
	 * between the old size and that block's end read as whatever was on disk
	 * unless they are zeroed — so skipping this leaks previously freed data
	 * into the file.
	 */
	pgoff_t index = (pgoff_t)(from >> PAGE_SHIFT);
	size_t offset = (size_t)(from & (PAGE_SIZE - 1));
	struct folio *folio;

	if (to <= from || !offset)
		return;
	folio = filemap_lock_folio(inode->i_mapping, index);
	if (!folio)
		return;
	folio_zero_range(folio, offset, PAGE_SIZE - offset);
	folio_unlock(folio);
	folio_put(folio);
}

/* ── inodes, names and attributes ───────────────────────────────── */

unsigned char fs_umode_to_ftype(umode_t mode)
{
	/* The ON-DISK type, which is a different numbering from the DT_* the
	 * getdents ABI uses. Confusing the two makes every directory listing
	 * report the wrong file types. */
	switch (mode & S_IFMT) {
	case S_IFREG:  return FT_REG_FILE;
	case S_IFDIR:  return FT_DIR;
	case S_IFCHR:  return FT_CHRDEV;
	case S_IFBLK:  return FT_BLKDEV;
	case S_IFIFO:  return FT_FIFO;
	case S_IFSOCK: return FT_SOCK;
	case S_IFLNK:  return FT_SYMLINK;
	default:       return FT_UNKNOWN;
	}
}

unsigned char fs_umode_to_dtype(umode_t mode)
{
	return (unsigned char)((mode & S_IFMT) >> 12);
}

unsigned char fs_ftype_to_dtype(unsigned int filetype)
{
	switch (filetype) {
	case FT_REG_FILE: return DT_REG;
	case FT_DIR:      return DT_DIR;
	case FT_CHRDEV:   return DT_CHR;
	case FT_BLKDEV:   return DT_BLK;
	case FT_FIFO:     return DT_FIFO;
	case FT_SOCK:     return DT_SOCK;
	case FT_SYMLINK:  return DT_LNK;
	default:          return DT_UNKNOWN;
	}
}

void fileattr_fill_flags(struct file_kattr *fa, u32 flags)
{
	memset(fa, 0, sizeof(*fa));
	fa->flags = flags;
	fa->flags_valid = true;
}

void fileattr_fill_xflags(struct file_kattr *fa, u32 xflags)
{
	memset(fa, 0, sizeof(*fa));
	fa->fsx_xflags = xflags;
	fa->fsx_valid = true;
}

void lock_two_nondirectories(struct inode *inode1, struct inode *inode2)
{
	/*
	 * Ordered by address, always. Two callers locking the same pair in
	 * opposite orders is the deadlock this exists to prevent, and the order
	 * has to be a property of the inodes rather than of the caller.
	 */
	if (inode1 > inode2) {
		struct inode *tmp = inode1;

		inode1 = inode2;
		inode2 = tmp;
	}
	if (inode1 && !S_ISDIR(inode1->i_mode))
		inode_lock(inode1);
	if (inode2 && inode2 != inode1 && !S_ISDIR(inode2->i_mode))
		inode_lock_nested(inode2, I_MUTEX_NONDIR2);
}

void unlock_two_nondirectories(struct inode *inode1, struct inode *inode2)
{
	if (inode1 && !S_ISDIR(inode1->i_mode))
		inode_unlock(inode1);
	if (inode2 && inode2 != inode1 && !S_ISDIR(inode2->i_mode))
		inode_unlock(inode2);
}

void simple_rename_timestamp(struct inode *old_dir, struct dentry *old_dentry,
                             struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *newino = d_inode(new_dentry);

	/* Both directories changed, and so did the ctime of what moved — a rename
	 * is a link and an unlink, and both halves are recorded. */
	old_dir->i_mtime = inode_set_ctime_current(old_dir);
	if (new_dir != old_dir)
		new_dir->i_mtime = inode_set_ctime_current(new_dir);
	inode_set_ctime_current(d_inode(old_dentry));
	if (newino)
		inode_set_ctime_current(newino);
}

static void page_put_link(void *arg)
{
	put_page((struct page *)arg);
}

/*
 * A symlink whose target is the first page of the inode's own data.
 *
 * This is btrfs's get_link — it stores the target as file data like any other
 * extent — so it has to be real. The page is held until the caller runs the
 * delayed call, because the string returned points into it.
 */
const char *page_get_link(struct dentry *dentry, struct inode *inode,
                          struct delayed_call *done)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page;
	char *kaddr;
	loff_t len;

	(void)dentry;
	page = read_mapping_page(mapping, 0, NULL);
	if (IS_ERR(page))
		return (const char *)page;

	set_delayed_call(done, page_put_link, page);
	kaddr = page_address(page);
	if (!kaddr) {
		put_page(page);
		clear_delayed_call(done);
		return ERR_PTR(-EIO);
	}
	/* Terminate it: the target is stored without a NUL, and a caller that
	 * ran off the end would read whatever the rest of the page holds. */
	len = i_size_read(inode);
	if (len > (loff_t)PAGE_SIZE - 1)
		len = PAGE_SIZE - 1;
	kaddr[len] = 0;
	return kaddr;
}

const char *simple_get_link(struct dentry *dentry, struct inode *inode,
                            struct delayed_call *done)
{
	(void)dentry; (void)done;
	return inode->i_link;
}

void kfree_link(void *p)
{
	kfree(p);
}

const struct inode_operations simple_dir_inode_operations;
const struct file_operations simple_dir_operations;
const struct qstr dotdot_name = QSTR_INIT((const unsigned char *)"..", 2);

long compat_ptr_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	/* Re-enters the native handler: none of these filesystems' ioctls take a
	 * pointer that needs converting, and b1nix has no 32-bit userspace on
	 * this architecture. */
	if (!file->f_op || !file->f_op->unlocked_ioctl)
		return -ENOIOCTLCMD;
	return file->f_op->unlocked_ioctl(file, cmd, arg);
}

unsigned long thp_get_unmapped_area(struct file *filp, unsigned long addr,
                                    unsigned long len, unsigned long pgoff,
                                    unsigned long flags)
{
	(void)filp; (void)len; (void)pgoff; (void)flags;
	/* No huge pages for file mappings, so no alignment to arrange; the
	 * caller's hint is returned unchanged. */
	return addr;
}

/* ── fiemap ─────────────────────────────────────────────────────── */

int fiemap_prep(struct inode *inode, struct fiemap_extent_info *fieinfo,
                u64 start, u64 *len, u32 supported_flags)
{
	u64 maxbytes = (u64)inode->i_sb->s_maxbytes;

	if (*len == 0)
		return -EINVAL;
	if (start >= maxbytes)
		return -EFBIG;
	if (*len > maxbytes || (maxbytes - *len) < start)
		*len = maxbytes - start;

	fieinfo->fi_flags &= supported_flags;
	if (fieinfo->fi_flags & ~supported_flags)
		return -EBADR;
	if (fieinfo->fi_flags & FIEMAP_FLAG_SYNC)
		filemap_write_and_wait(inode->i_mapping);
	return 0;
}

int fiemap_fill_next_extent(struct fiemap_extent_info *fieinfo, u64 logical,
                            u64 phys, u64 len, u32 flags)
{
	struct fiemap_extent extent;

	/* Room is the caller's, and running past it overruns a userspace buffer —
	 * which is why the count is checked before the copy and not after. */
	if (fieinfo->fi_extents_mapped >= fieinfo->fi_extents_max)
		return 1;

	memset(&extent, 0, sizeof(extent));
	extent.fe_logical = logical;
	extent.fe_physical = phys;
	extent.fe_length = len;
	extent.fe_flags = flags;

	if (copy_to_user(fieinfo->fi_extents_start + fieinfo->fi_extents_mapped,
	                 &extent, sizeof(extent)))
		return -EFAULT;
	fieinfo->fi_extents_mapped++;
	return fieinfo->fi_extents_mapped == fieinfo->fi_extents_max ? 1 : 0;
}

/* ── odds and ends ──────────────────────────────────────────────── */

void seq_escape(struct seq_file *m, const char *s, const char *esc)
{
	/* Mount options go through it: a device name containing a space or a
	 * comma would otherwise produce a /proc/mounts line that cannot be parsed
	 * back. */
	for (; *s; s++) {
		const char *e;

		for (e = esc; *e; e++)
			if (*s == *e)
				break;
		if (*e)
			seq_printf(m, "\\%03o", (unsigned char)*s);
		else
			seq_putc(m, *s);
	}
}

void seq_escape_str(struct seq_file *m, const char *src, unsigned int flags,
                    const char *esc)
{
	(void)flags;
	seq_escape(m, src, esc);
}

int cond_resched_rwlock_write(void *lock)
{
	(void)lock;
	/* Yielding here would mean dropping the caller's lock, which only the
	 * caller can do safely. Reporting that nothing was dropped is the answer
	 * that keeps the caller's state valid. */
	return 0;
}

int cond_resched_rwlock_read(void *lock)
{
	(void)lock;
	return 0;
}

int refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock)
{
	/* The lock is taken first and the decrement made under it: that is what
	 * makes the "did it reach zero" decision single, and the cost is holding
	 * the mutex for one atomic. */
	mutex_lock(lock);
	if (refcount_dec_and_test(r))
		return 1;
	mutex_unlock(lock);
	return 0;
}

void list_splice_init_rcu(struct list_head *list, struct list_head *head,
                          void (*sync)(void))
{
	if (list_empty(list))
		return;
	/* The sync is what makes the splice safe for a concurrent reader: the
	 * destination is published only once every reader that could be walking
	 * the old list has finished. */
	if (sync)
		sync();
	list_splice_init(list, head);
}

unsigned int work_busy(struct work_struct *work)
{
	(void)work;
	return 0;
}

int vmf_error(int err)
{
	/*
	 * Map an errno onto a fault result. Getting this wrong turns a transient
	 * failure into a killed process: -ENOMEM must become OOM (which the
	 * caller retries after reclaim), and only genuinely unrecoverable errors
	 * become SIGBUS.
	 */
	if (err == -ENOMEM)
		return VM_FAULT_OOM;
	if (err == -EHWPOISON)
		return VM_FAULT_HWPOISON;
	return VM_FAULT_SIGBUS;
}

int vmf_fs_error(int err)
{
	if (err == 0)
		return VM_FAULT_LOCKED;
	if (err == -EAGAIN || err == -ENOMEM)
		return VM_FAULT_OOM;
	return VM_FAULT_SIGBUS;
}

/* ── the gaps ───────────────────────────────────────────────────── */

/*
 * RAID-6 parity, and RAID-5's XOR.
 *
 * lib/raid6 is not imported (see docs/filesystems-and-storage.md), so these have no
 * implementation. They PANIC rather than doing nothing: a silent no-op would
 * write a stripe whose parity is zeros, which reads back fine until a disk
 * fails and then reconstructs garbage. Failing at the first raid5/raid6 write
 * is the only safe behaviour.
 *
 * Every other btrfs profile — single, dup, raid0, raid1, raid10, which is what
 * mkfs.btrfs uses by default — never reaches this.
 */
void raid6_call_gen_syndrome(int disks, size_t bytes, void **ptrs)
{
	(void)disks; (void)bytes; (void)ptrs;
	panic("lkpi: btrfs raid6 needs lib/raid6, which is not imported");
}

void raid6_2data_recov(int disks, size_t bytes, int faila, int failb,
                       void **ptrs)
{
	(void)disks; (void)bytes; (void)faila; (void)failb; (void)ptrs;
	panic("lkpi: btrfs raid6 recovery needs lib/raid6, which is not imported");
}

void raid6_datap_recov(int disks, size_t bytes, int faila, void **ptrs)
{
	(void)disks; (void)bytes; (void)faila; (void)ptrs;
	panic("lkpi: btrfs raid6 recovery needs lib/raid6, which is not imported");
}

const struct raid6_calls raid6_call = {
	.gen_syndrome = raid6_call_gen_syndrome,
	.name = "unimplemented",
};

void xor_blocks(unsigned int count, unsigned int bytes, void *dest, void **srcs)
{
	(void)count; (void)bytes; (void)dest; (void)srcs;
	panic("lkpi: btrfs raid5 needs lib/raid, which is not imported");
}

/*
 * btrfs's zoned completion.
 *
 * Defined in its zoned.c under CONFIG_BLK_DEV_ZONED, which is off — but the
 * caller references it unconditionally. No device here is zoned
 * (`bdev_is_zoned` is always false), so it cannot be reached; if it ever is,
 * that is a bug in the zoned check rather than a missing feature.
 */
void btrfs_finish_ordered_zoned(void *ordered)
{
	(void)ordered;
	panic("lkpi: btrfs zoned completion reached on a kernel with no zoned "
	      "devices");
}

/* ── slab bulk operations ───────────────────────────────────────── */

int kmem_cache_alloc_bulk(struct kmem_cache *s, gfp_t gfp, size_t nr, void **p)
{
	size_t i;

	/* A short return is not an error — the caller loops — but a partial
	 * failure must not leave objects allocated that the caller does not know
	 * about, so what was taken is returned. */
	for (i = 0; i < nr; i++) {
		p[i] = kmem_cache_alloc(s, gfp);
		if (!p[i])
			break;
	}
	return (int)i;
}

void kmem_cache_free_bulk(struct kmem_cache *s, size_t nr, void **p)
{
	size_t i;

	for (i = 0; i < nr; i++)
		if (p[i])
			kmem_cache_free(s, p[i]);
}

/* ── waiting ────────────────────────────────────────────────────── */

int prepare_to_wait_event(struct wait_queue_head *wq,
                          struct wait_queue_entry *entry, int state)
{
	(void)state;
	/*
	 * Publishes the waiter before the condition is re-tested, which is what
	 * makes a wake arriving in between visible rather than lost.
	 *
	 * Returns 0 always: nothing here interrupts a filesystem waiter, so there
	 * is no -ERESTARTSYS to report. A caller that checks for one still
	 * compiles and simply never sees it.
	 */
	add_wait_queue(wq, entry);
	return 0;
}

void prepare_to_wait_exclusive(struct wait_queue_head *wq,
                               struct wait_queue_entry *entry, int state)
{
	(void)state;
	entry->flags |= WQ_FLAG_EXCLUSIVE;
	add_wait_queue(wq, entry);
}

/* finish_wait is already defined in <linux/wait.h> as an inline over
 * remove_wait_queue. */

void workqueue_set_max_active(struct workqueue_struct *wq, int max_active)
{
	(void)wq;
	(void)max_active;
	/*
	 * btrfs tunes its pools as they grow. lkpi's workqueue runs items on one
	 * thread in submission order and has no width to change — so this is a
	 * hint that goes nowhere, and the consequence is that btrfs's thread
	 * pools do not widen under load.
	 */
}

/* The buffer-head write helpers live in kernel/lkpi/fs_buffer.c, which is
 * built with the filesystem import: btrfs reaches them only on its
 * inline-data path, and ext4 is built on them throughout. */

/* ── 6.x VFS helpers ────────────────────────────────────────────── */

/* Built without CONFIG_UNICODE or CONFIG_FS_ENCRYPTION, so there are no
 * casefold or encrypted-name dentry operations to install. */
void generic_set_sb_d_ops(struct super_block *sb)
{
	(void)sb;
}

/* Record that the atomic-write fields were considered; the capability itself
 * is reported only when the device has a unit size, which none here does. */
void generic_fill_statx_atomic_writes(struct kstat *stat, unsigned int unit_min,
                                      unsigned int unit_max,
                                      unsigned int unit_max_opt)
{
	stat->result_mask |= STATX_WRITE_ATOMIC;
	if (unit_min) {
		stat->atomic_write_unit_min = unit_min;
		stat->atomic_write_unit_max = unit_max;
		stat->atomic_write_unit_max_opt = unit_max_opt;
		stat->atomic_write_segments_max = 1;
	}
}

int generic_atomic_write_valid(struct kiocb *iocb, struct iov_iter *iter)
{
	size_t len = iov_iter_count(iter);

	if (!is_power_of_2(len))
		return -EINVAL;
	if (!IS_ALIGNED(iocb->ki_pos, len))
		return -EINVAL;
	if (!(iocb->ki_flags & IOCB_DIRECT))
		return -EOPNOTSUPP;
	return 0;
}

/* The 6.8 names of __mnt_want_write/__mnt_drop_write. */
int mnt_get_write_access(struct vfsmount *mnt)
{
	return __mnt_want_write(mnt);
}

void mnt_put_write_access(struct vfsmount *mnt)
{
	__mnt_drop_write(mnt);
}

/* No stacking filesystem here opens backing files. */
const struct path *backing_file_user_path(const struct file *f)
{
	return &f->f_path;
}

int __sysfs_match_string(const char * const *array, size_t n, const char *str)
{
	size_t index;

	for (index = 0; index < n; index++) {
		const char *item = array[index];

		if (!item)
			break;
		if (sysfs_streq(item, str))
			return (int)index;
	}
	return -EINVAL;
}

/* export_operations->encode_fh for 32-bit inode numbers: (ino, generation),
 * plus the parent's when one is asked for. Linux's own, from fs/libfs.c. */
int generic_encode_ino32_fh(struct inode *inode, __u32 *fh, int *max_len,
                            struct inode *parent)
{
	struct fid *fid = (void *)fh;
	int len = *max_len;
	int type = FILEID_INO32_GEN;

	if (parent && (len < 4)) {
		*max_len = 4;
		return FILEID_INVALID;
	} else if (len < 2) {
		*max_len = 2;
		return FILEID_INVALID;
	}

	len = 2;
	fid->i32.ino = (u32)inode->i_ino;
	fid->i32.gen = inode->i_generation;
	if (parent) {
		fid->i32.parent_ino = (u32)parent->i_ino;
		fid->i32.parent_gen = parent->i_generation;
		len = 4;
		type = FILEID_INO32_GEN_PARENT;
	}
	*max_len = len;
	return type;
}
