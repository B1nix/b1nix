/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PAGEMAP_H
#define LKPI_LINUX_PAGEMAP_H

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/errno.h>
#include <linux/xarray.h>

/*
 * The page cache.
 *
 * A mapping is an index from file offset to page, and this is the interface to
 * it. Three things about the model have to be right or a filesystem corrupts
 * data rather than failing:
 *
 *   - A page found in the cache is not necessarily readable. `Uptodate` says
 *     whether its contents match the disk; a page that has just been inserted
 *     is present, locked, and empty.
 *   - The page lock is what serialises everything: filling a page, writing it
 *     back, truncating it away. `folio_lock` sleeps, and a caller that holds it
 *     across an allocation must not allocate with __GFP_FS.
 *   - A reference keeps the page alive; the lock keeps its CONTENTS still.
 *     They are separate and both are needed.
 *
 * The `__filemap_get_folio` flags are how a caller says what it wants — find
 * only, create if absent, return it locked — and mixing them up is how a path
 * ends up either allocating a page it should have found or using one it never
 * locked.
 */

struct address_space;
struct folio;
struct page;
struct file;
struct readahead_control;
/* At file scope, so the type in the prototypes below is the one <linux/fs.h>
 * defines. A struct first seen inside a parameter list belongs to that
 * prototype's scope and its pointer then refuses to match the real one — which
 * shows up as "incompatible pointer types passing struct file_ra_state * to
 * parameter of type struct file_ra_state *". */
struct file_ra_state;
struct writeback_control;
struct vm_fault;
struct vm_area_struct;
struct inode;

#define FGP_ACCESSED       0x00000001
#define FGP_LOCK           0x00000002
#define FGP_CREAT          0x00000004
#define FGP_WRITE          0x00000008
#define FGP_NOFS           0x00000010
#define FGP_NOWAIT         0x00000020
#define FGP_FOR_MMAP       0x00000040
#define FGP_STABLE         0x00000080
#define FGP_WRITEBEGIN     (FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE)

typedef unsigned int fgf_t;

/*
 * Ask __filemap_get_folio for a folio big enough to hold `size` bytes.
 *
 * Upstream encodes the order in the high bits of the flags. One page per folio
 * here, so the request cannot be honoured and the encoding would be a promise
 * this page cache does not keep — it returns the flags unchanged, and the
 * caller gets a single-page folio, which is what every caller already handles
 * (it checks the folio's actual size rather than assuming).
 */
static inline fgf_t fgf_set_order(size_t size)
{
	(void)size;
	return 0;
}

static inline gfp_t mapping_gfp_mask(struct address_space *mapping)
{
	return mapping ? mapping->gfp_mask : GFP_KERNEL;
}

static inline void mapping_set_gfp_mask(struct address_space *m, gfp_t mask)
{
	if (m)
		m->gfp_mask = mask;
}

static inline gfp_t mapping_gfp_constraint(struct address_space *mapping,
                                           gfp_t gfp_mask)
{
	return mapping_gfp_mask(mapping) & gfp_mask;
}

static inline void mapping_set_unevictable(struct address_space *m) { (void)m; }
static inline void mapping_clear_unevictable(struct address_space *m) { (void)m; }
static inline void mapping_set_large_folios(struct address_space *m) { (void)m; }
static inline bool mapping_large_folio_support(struct address_space *m)
{ (void)m; return false; }

static inline void mapping_set_release_always(struct address_space *m)
{ set_bit(AS_RELEASE_ALWAYS, &m->flags); }
static inline void mapping_set_stable_writes(struct address_space *m)
{ set_bit(AS_STABLE_WRITES, &m->flags); }
static inline bool mapping_stable_writes(const struct address_space *m)
{ return test_bit(AS_STABLE_WRITES, &((struct address_space *)m)->flags); }

/* How many pages a mapping of this size needs, and the index of an offset. */
static inline pgoff_t linear_page_index(struct vm_area_struct *vma,
                                        unsigned long address);

/* ── finding and creating ───────────────────────────────────────── */

struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index);
struct folio *filemap_lock_folio(struct address_space *mapping, pgoff_t index);
struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index,
                                  fgf_t fgp_flags, gfp_t gfp);
struct page *pagecache_get_page(struct address_space *mapping, pgoff_t index,
                                fgf_t fgp_flags, gfp_t gfp);
struct page *find_get_page(struct address_space *mapping, pgoff_t offset);
struct page *find_lock_page(struct address_space *mapping, pgoff_t index);
struct page *find_or_create_page(struct address_space *mapping, pgoff_t index,
                                 gfp_t gfp_mask);
struct page *grab_cache_page_write_begin(struct address_space *mapping,
                                         pgoff_t index);
struct page *read_cache_page(struct address_space *mapping, pgoff_t index,
                             int (*filler)(struct file *, struct folio *),
                             struct file *file);
struct page *read_cache_page_gfp(struct address_space *mapping, pgoff_t index,
                                 gfp_t gfp);
struct folio *read_cache_folio(struct address_space *mapping, pgoff_t index,
                               int (*filler)(struct file *, struct folio *),
                               struct file *file);
/* Read a folio in and return it uptodate, or an ERR_PTR. The difference from
 * `filemap_get_folio` is that this one performs the I/O — a caller that used
 * the other and read the bytes would be reading an empty page. */
struct folio *read_mapping_folio(struct address_space *mapping, pgoff_t index,
                                 struct file *file);
struct page *read_mapping_page(struct address_space *mapping, pgoff_t index,
                               struct file *file);
size_t memcpy_from_file_folio(char *to, struct folio *folio, loff_t pos,
                              size_t len);

/*
 * A file grew and the last partial block was not zeroed on disk.
 *
 * Called after i_size increases past the end of a block that already existed:
 * the bytes between the old size and the block's end read as whatever was on
 * disk unless they are zeroed. Skipping it leaks previously freed data into the
 * file.
 */
void pagecache_isize_extended(struct inode *inode, loff_t from, loff_t to);

struct folio *filemap_grab_folio(struct address_space *mapping, pgoff_t index);
struct folio *filemap_alloc_folio(gfp_t gfp, unsigned int order);

int filemap_add_folio(struct address_space *mapping, struct folio *folio,
                      pgoff_t index, gfp_t gfp);
int add_to_page_cache_lru(struct page *page, struct address_space *mapping,
                          pgoff_t index, gfp_t gfp);
void filemap_remove_folio(struct folio *folio);
void delete_from_page_cache(struct page *page);
void filemap_invalidate_lock(struct address_space *mapping);
void filemap_invalidate_unlock(struct address_space *mapping);
void filemap_invalidate_lock_shared(struct address_space *mapping);
void filemap_invalidate_unlock_shared(struct address_space *mapping);
int filemap_check_errors(struct address_space *mapping);
int invalidate_inode_pages2(struct address_space *mapping);
int invalidate_inode_pages2_range(struct address_space *mapping, pgoff_t start,
                                  pgoff_t end);
unsigned long invalidate_mapping_pages(struct address_space *mapping,
                                       pgoff_t start, pgoff_t end);

/* ── batches ────────────────────────────────────────────────────── */

/*
 * Collect a run of folios in one pass over the index.
 *
 * The `_tag` form filters by an xarray tag — DIRTY or WRITEBACK — which is how
 * writeback walks only the pages it has to. Walking every page and testing the
 * flag gives the same answer and takes time proportional to the file rather
 * than to the dirty part of it.
 */
/*
 * The page-cache tags ARE xarray marks, not integers: a filesystem assigns one
 * to an `xa_mark_t` variable and passes it on. Declaring them as ints compiles
 * here and fails at every such assignment.
 */
#define PAGECACHE_TAG_DIRTY     XA_MARK_0
#define PAGECACHE_TAG_WRITEBACK XA_MARK_1
#define PAGECACHE_TAG_TOWRITE   XA_MARK_2

unsigned filemap_get_folios(struct address_space *mapping, pgoff_t *start,
                            pgoff_t end, struct folio_batch *fbatch);
unsigned filemap_get_folios_tag(struct address_space *mapping, pgoff_t *start,
                                pgoff_t end, xa_mark_t tag,
                                struct folio_batch *fbatch);
unsigned filemap_get_folios_contig(struct address_space *mapping,
                                   pgoff_t *start, pgoff_t end,
                                   struct folio_batch *fbatch);

/* ── locking and waiting ────────────────────────────────────────── */

/* The uptodate marks. Setting it is what makes a folio readable, so it happens
 * exactly once, after the last byte is in place — a filesystem that sets it
 * before the read completes hands out uninitialised memory. */
void folio_mark_uptodate(struct folio *folio);
void folio_end_read(struct folio *folio, bool success);
void zero_user_segment(struct page *page, unsigned start, unsigned end);
void zero_user_segments(struct page *page, unsigned s1, unsigned e1,
                        unsigned s2, unsigned e2);
void memzero_page(struct page *page, size_t offset, size_t len);
void memcpy_to_page(struct page *page, size_t offset, const char *from,
                    size_t len);
void memcpy_from_page(char *to, struct page *page, size_t offset, size_t len);
void folio_zero_range(struct folio *folio, size_t start, size_t length);
void folio_zero_segment(struct folio *folio, size_t start, size_t end);
void folio_zero_segments(struct folio *folio, size_t s1, size_t e1,
                         size_t s2, size_t e2);

void folio_lock(struct folio *folio);
int folio_trylock(struct folio *folio);
void folio_unlock(struct folio *folio);
void folio_wait_locked(struct folio *folio);
void folio_wait_stable(struct folio *folio);
void folio_wait_bit(struct folio *folio, int bit_nr);
void folio_get(struct folio *folio);
void folio_put(struct folio *folio);
bool folio_put_testzero(struct folio *folio);

void lock_page(struct page *page);
int trylock_page(struct page *page);
void unlock_page(struct page *page);
void wait_on_page_writeback(struct page *page);
void wait_on_page_locked(struct page *page);
void end_page_writeback(struct page *page);
/* get_page and put_page are declared in <lkpi/page.h>, which <linux/mm.h>
 * brings in: put_page there returns whether the put freed the frame, and a
 * second declaration returning void is a conflict rather than an overload. */

/*
 * The one asymmetry worth writing down: `folio_lock` sleeps and may be called
 * with no locks held, while `folio_trylock` may be called from anywhere. A
 * caller holding a spinlock must use the second — b1nix panics on a sleep under
 * a spinlock rather than deadlocking silently, which is the better failure but
 * still a failure.
 */

/* ── read-ahead ─────────────────────────────────────────────────── */

struct readahead_control {
	struct file *file;
	struct address_space *mapping;
	struct file_ra_state *ra;
	pgoff_t _index;
	unsigned int _nr_pages;
	unsigned int _batch_count;
};

#define DEFINE_READAHEAD(ractl, f, r, m, i)                                    \
	struct readahead_control ractl = {                                         \
		.file = f, .mapping = m, .ra = r, ._index = i }

static inline loff_t readahead_pos(struct readahead_control *rac)
{ return (loff_t)rac->_index << PAGE_SHIFT; }
static inline size_t readahead_length(struct readahead_control *rac)
{ return (size_t)rac->_nr_pages << PAGE_SHIFT; }
static inline pgoff_t readahead_index(struct readahead_control *rac)
{ return rac->_index; }
static inline unsigned int readahead_count(struct readahead_control *rac)
{ return rac->_nr_pages; }

struct folio *readahead_folio(struct readahead_control *rac);
struct page *readahead_page(struct readahead_control *rac);
void page_cache_ra_unbounded(struct readahead_control *rac,
                             unsigned long nr_to_read, unsigned long lookahead);
void page_cache_sync_readahead(struct address_space *mapping,
                               struct file_ra_state *ra, struct file *file,
                               pgoff_t index, unsigned long req_count);
void page_cache_async_readahead(struct address_space *mapping,
                                struct file_ra_state *ra, struct file *file,
                                struct folio *folio, unsigned long req_count);

#define readahead_for_each(rac, folio)                                         \
	while (((folio) = readahead_folio(rac)) != NULL)

/* ── dirtying and writeback ─────────────────────────────────────── */

bool filemap_dirty_folio(struct address_space *mapping, struct folio *folio);
bool folio_clear_dirty_for_io(struct folio *folio);
void folio_redirty_for_writepage(struct writeback_control *wbc,
                                 struct folio *folio);
int filemap_write_and_wait(struct address_space *mapping);
int write_one_page(struct page *page);
int __set_page_dirty_nobuffers(struct page *page);
void account_page_redirty(struct page *page);
void tag_pages_for_writeback(struct address_space *mapping, pgoff_t start,
                             pgoff_t end);

/* Whole-file offsets from an index and back. Named as upstream names them
 * because a filesystem mixes both units in one expression constantly. */
static inline loff_t page_file_offset(struct page *page)
{ return (loff_t)page->index << PAGE_SHIFT; }

#define page_cache_next_miss(mapping, index, max) (index)

int filemap_fault(struct vm_fault *vmf);
vm_fault_t filemap_map_pages(struct vm_fault *vmf, pgoff_t start, pgoff_t end);
int filemap_page_mkwrite(struct vm_fault *vmf);

/* ── the rest of the page-cache surface ─────────────────────────── */

/*
 * Reads, writes and the errors they leave behind.
 *
 * `filemap_check_wb_err` and `errseq_check_and_advance` are the two halves of
 * the once-per-opener error report: the first looks without consuming, the
 * second consumes. Using the consuming form where a peek was meant makes the
 * error disappear for the reader who should have got it.
 */
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter,
                     ssize_t already_read);
ssize_t filemap_splice_read(struct file *in, loff_t *ppos,
                            struct pipe_inode_info *pipe, size_t len,
                            unsigned int flags);
int filemap_check_wb_err(struct address_space *mapping, errseq_t since);
int filemap_fdatawrite_wbc(struct address_space *mapping,
                           struct writeback_control *wbc);
int filemap_fdatawait_range_keep_errors(struct address_space *mapping,
                                        loff_t start_byte, loff_t end_byte);
bool filemap_range_has_page(struct address_space *mapping, loff_t start,
                            loff_t end);
bool filemap_range_needs_writeback(struct address_space *mapping, loff_t start,
                                   loff_t end);
bool filemap_release_folio(struct folio *folio, gfp_t gfp);
loff_t mapping_seek_hole_data(struct address_space *mapping, loff_t start,
                              loff_t end, int whence);
bool mapping_tagged(struct address_space *mapping, xa_mark_t tag);
bool mapping_writably_mapped(struct address_space *mapping);
struct page *__page_cache_alloc(gfp_t gfp);
struct page *find_get_page_flags(struct address_space *mapping, pgoff_t offset,
                                 fgf_t fgp_flags);
/* Fills the caller's array up to its own capacity, which upstream takes from
 * the array's declared size at the call site — so the count is not a
 * parameter. */
unsigned readahead_page_batch(struct readahead_control *rac,
                              struct page **array);
size_t readahead_batch_length(struct readahead_control *rac);

/* Folio state a filesystem inspects or changes. */
bool folio_mapped(struct folio *folio);
bool folio_maybe_dma_pinned(struct folio *folio);
bool folio_test_large(struct folio *folio);
void folio_cancel_dirty(struct folio *folio);
void folio_invalidate(struct folio *folio, size_t offset, size_t length);
bool __folio_start_writeback(struct folio *folio, bool keep_write);
static inline pgoff_t folio_next_index(struct folio *folio)
{ return folio_index(folio) + (pgoff_t)folio_nr_pages(folio); }
/*
 * How much of this folio is still inside the file.
 *
 * Returns the bytes usable, or 0 when the folio is entirely past EOF — a page
 * fault on a file that shrank under it has to see that rather than write into
 * a page nothing owns any more.
 */
size_t folio_mkwrite_check_truncate(struct folio *folio, struct inode *inode);
void folio_set_bh(struct buffer_head *bh, struct folio *folio,
                  unsigned long offset);

int redirty_page_for_writepage(struct writeback_control *wbc,
                               struct page *page);
bool noop_dirty_folio(struct address_space *mapping, struct folio *folio);
ssize_t noop_direct_IO(struct kiocb *iocb, struct iov_iter *iter);
bool block_is_partially_uptodate(struct folio *folio, size_t from,
                                 size_t count);

/*
 * The largest folio the page cache will build. One page here, so the constant
 * is 0 (order zero) rather than a size — callers use it as an order.
 */
#define MAX_PAGECACHE_ORDER 0

/* Turn an errno into a fault return. `vmf_fs_error` maps the filesystem errors
 * a fault can raise onto the VM_FAULT_* the fault handler must return —
 * -ENOMEM becomes OOM, -EAGAIN becomes RETRY, everything else SIGBUS. Getting
 * that wrong turns a transient failure into a killed process. */
int vmf_error(int err);
int vmf_fs_error(int err);
#define VM_FAULT_LOCKED 0x0200

void flush_dcache_page(struct page *page);
void flush_dcache_folio(struct folio *folio);

/* Uncached buffered I/O (RWF_DONTCACHE): a folio dropped once written. */
#define FGP_DONTCACHE      0x00000100

static inline bool folio_contains(const struct folio *folio, pgoff_t index)
{ return index - folio->index < folio_nr_pages((struct folio *)folio); }

/* No large folios in this page cache: every mapping is order 0. */
static inline unsigned int mapping_max_folio_order(const struct address_space *mapping)
{ (void)mapping; return 0; }
static inline size_t mapping_max_folio_size(const struct address_space *mapping)
{ return PAGE_SIZE << mapping_max_folio_order(mapping); }
static inline void mapping_set_folio_order_range(struct address_space *mapping,
                                                 unsigned int min, unsigned int max)
{ (void)mapping; (void)min; (void)max; }
static inline void mapping_set_folio_min_order(struct address_space *mapping,
                                               unsigned int min)
{ (void)mapping; (void)min; }
static inline void mapping_clear_stable_writes(struct address_space *mapping)
{ clear_bit(AS_STABLE_WRITES, &mapping->flags); }

/* The folio a buffered write copies into, locked and in the cache. */
static inline struct folio *write_begin_get_folio(const struct kiocb *iocb,
                                                  struct address_space *mapping,
                                                  pgoff_t index, size_t len)
{
	fgf_t fgp_flags = FGP_WRITEBEGIN;

	(void)len;
	if (iocb && (iocb->ki_flags & IOCB_DONTCACHE))
		fgp_flags |= FGP_DONTCACHE;
	return __filemap_get_folio(mapping, index, fgp_flags,
	                           mapping_gfp_mask(mapping));
}

/* A folio of 2^order pages outside any mapping. Only order 0 exists here. */
static inline struct folio *folio_alloc(gfp_t gfp, unsigned int order)
{ return order ? NULL : filemap_alloc_folio(gfp, 0); }

int generic_error_remove_folio(struct address_space *mapping, struct folio *folio);

/* Flush (optionally), then drop every cached folio in [start, end]. */
int filemap_invalidate_inode(struct inode *inode, bool flush, loff_t start,
                             loff_t end);

/*
 * Grow a readahead window to cover more of the file. The caller must cope
 * with getting less than it asked for — upstream stops at the first folio it
 * cannot add — and here the window is never grown: btrfs asks for it to cover
 * a whole compressed extent, and the pages it leaves out are read on demand.
 */
static inline void readahead_expand(struct readahead_control *ractl,
                                    loff_t new_start, size_t new_len)
{ (void)ractl; (void)new_start; (void)new_len; }

#endif
