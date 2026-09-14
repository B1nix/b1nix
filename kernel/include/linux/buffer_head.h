/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BUFFER_HEAD_H
#define LKPI_LINUX_BUFFER_HEAD_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/bit_spinlock.h>
#include <linux/blk_types.h>
#include <linux/fs.h>
#include <linux/wait.h>
/* The folio operations a buffer head is built on — folio_lock, folio_unlock,
 * the uptodate marks. Upstream's buffer_head.h includes it for the same reason:
 * a buffer belongs to a folio and every buffer operation touches it. */
#include <linux/pagemap.h>

/*
 * The buffer head: one filesystem block, mapped to one device block, held in a
 * page.
 *
 * ext4 and jbd2 are built on this and btrfs is not, which is the whole reason
 * the interface still exists. A journal is a list of blocks with state — this
 * one is dirty, this one is being written, this one belongs to the running
 * transaction — and `b_state` is where that state lives.
 *
 * The state bits are not decoration. `BH_Uptodate` means the buffer's contents
 * match the disk; `BH_Dirty` means they do not and the disk is the stale one;
 * `BH_Lock` means an I/O is in flight. jbd2 adds its own on top
 * (`BH_JBD`, `BH_Shadow`, `BH_Revoked`), and the numbering has to leave room
 * for them — which is what `BH_PrivateStart` is.
 *
 * `b_state` is also the word `bit_spin_lock` locks for the buffer's own list,
 * so bit zero of it is spoken for by more than one user. That is upstream's
 * arrangement, not a coincidence to tidy up.
 */

enum bh_state_bits {
	BH_Uptodate,      /* contents match the disk */
	BH_Dirty,         /* contents differ from the disk and must be written */
	BH_Lock,          /* I/O in flight; wait on it before touching b_data */
	BH_Req,           /* has been submitted at least once */
	BH_Mapped,        /* b_blocknr is valid */
	BH_New,           /* freshly allocated, contents undefined on disk */
	BH_Async_Read,
	BH_Async_Write,
	BH_Delay,         /* allocated on disk lazily; block not assigned yet */
	BH_Boundary,
	BH_Write_EIO,
	BH_Unwritten,     /* allocated but never written; reads as zeros */
	BH_Quiet,
	BH_Meta,
	BH_Prio,
	BH_Defer_Completion,
	BH_PrivateStart,  /* filesystems and jbd2 number their own bits from here */
};

struct buffer_head;
struct page;
struct folio;
struct block_device;
struct inode;

typedef void (bh_end_io_t)(struct buffer_head *bh, int uptodate);

struct buffer_head {
	unsigned long b_state;
	struct buffer_head *b_this_page; /* the other buffers in the same folio */
	union {
		struct page *b_page;
		struct folio *b_folio;
	};

	sector_t b_blocknr;   /* block number on b_bdev */
	size_t b_size;
	char *b_data;         /* the bytes, inside the folio */

	struct block_device *b_bdev;
	bh_end_io_t *b_end_io;
	void *b_private;
	struct list_head b_assoc_buffers;
	struct address_space *b_assoc_map;
	atomic_t b_count;
	/*
	 * Guards the transition of BH_Uptodate on the LAST buffer of a folio.
	 *
	 * The folio becomes uptodate when every buffer in it is, and that decision
	 * is made by whichever completion finishes last — so two completions
	 * finishing at once must not both decide they were last, or neither marks
	 * the folio and a reader waits forever. jbd2 and ext4 take this lock
	 * directly, which is why it is spelled as upstream spells it.
	 */
	spinlock_t b_uptodate_lock;
	/* jbd2's own state lock over b_private. */
	int b_state_lock;
};

/*
 * The state-bit accessors, generated the way upstream generates them.
 *
 * Three forms per bit and each is a different promise: `set_` and `clear_` are
 * atomic read-modify-writes, the `__`-prefixed forms are not (for a buffer
 * nobody else can see yet), and `buffer_` tests. Writing them out by hand would
 * be sixty near-identical functions and sixty chances for a typo to set the
 * wrong bit — which is a class of bug that produces a filesystem that looks
 * fine until a crash.
 */
#define BUFFER_FNS(bit, name)                                                  \
static inline void set_buffer_##name(struct buffer_head *bh)                   \
{ set_bit(BH_##bit, &(bh)->b_state); }                                         \
static inline void clear_buffer_##name(struct buffer_head *bh)                 \
{ clear_bit(BH_##bit, &(bh)->b_state); }                                       \
static inline int buffer_##name(const struct buffer_head *bh)                  \
{ return test_bit(BH_##bit, &((struct buffer_head *)bh)->b_state); }

#define TAS_BUFFER_FNS(bit, name)                                              \
static inline int test_set_buffer_##name(struct buffer_head *bh)               \
{ return test_and_set_bit(BH_##bit, &(bh)->b_state); }                         \
static inline int test_clear_buffer_##name(struct buffer_head *bh)             \
{ return test_and_clear_bit(BH_##bit, &(bh)->b_state); }

BUFFER_FNS(Uptodate, uptodate)
BUFFER_FNS(Dirty, dirty)
TAS_BUFFER_FNS(Dirty, dirty)
BUFFER_FNS(Lock, locked)
BUFFER_FNS(Req, req)
TAS_BUFFER_FNS(Req, req)
BUFFER_FNS(Mapped, mapped)
BUFFER_FNS(New, new)
BUFFER_FNS(Async_Read, async_read)
BUFFER_FNS(Async_Write, async_write)
TAS_BUFFER_FNS(Async_Write, async_write)
BUFFER_FNS(Delay, delay)
BUFFER_FNS(Boundary, boundary)
BUFFER_FNS(Write_EIO, write_io_error)
BUFFER_FNS(Unwritten, unwritten)
BUFFER_FNS(Meta, meta)
BUFFER_FNS(Prio, prio)
BUFFER_FNS(Defer_Completion, defer_completion)

static inline void set_buffer_uptodate_nolock(struct buffer_head *bh)
{
	__set_bit(BH_Uptodate, &bh->b_state);
}

/* Where in its folio this buffer's bytes start. */
static inline unsigned long bh_offset(const struct buffer_head *bh)
{
	return (unsigned long)bh->b_data & (PAGE_SIZE - 1);
}

/* The most buffers a page can be divided into: one per 512-byte sector. */
#define MAX_BUF_PER_PAGE (PAGE_SIZE / 512)

/* How many filesystem blocks fit in one folio. Zero would be a division by
 * zero at every call site, so a block larger than a page — which neither
 * filesystem supports here — is a build-time impossibility rather than a
 * run-time one. */
static inline unsigned int i_blocks_per_folio(struct inode *inode,
                                              struct folio *folio)
{
	(void)folio;
	return (unsigned int)(PAGE_SIZE >> inode->i_blkbits);
}
static inline unsigned int i_blocks_per_page(struct inode *inode,
                                             struct page *page)
{
	(void)page;
	return (unsigned int)(PAGE_SIZE >> inode->i_blkbits);
}

#define page_buffers(page)   ((struct buffer_head *)page_private(page))
#define page_has_buffers(page) PagePrivate(page)
struct buffer_head *folio_buffers(struct folio *folio);

/* ── references and locking ─────────────────────────────────────── */

static inline void get_bh(struct buffer_head *bh) { atomic_inc(&bh->b_count); }
static inline void put_bh(struct buffer_head *bh) { atomic_dec(&bh->b_count); }

void __brelse(struct buffer_head *bh);
static inline void brelse(struct buffer_head *bh)
{
	if (bh)
		__brelse(bh);
}
void __bforget(struct buffer_head *bh);
static inline void bforget(struct buffer_head *bh)
{
	if (bh)
		__bforget(bh);
}

void lock_buffer(struct buffer_head *bh);
void unlock_buffer(struct buffer_head *bh);
int trylock_buffer(struct buffer_head *bh);
void wait_on_buffer(struct buffer_head *bh);
/* Take the lock unless the buffer is already up to date. Returns 1 when it is
 * up to date and unlocked (nothing to do), 0 when the caller now holds it and
 * must read. The two-valued answer is what stops a second reader issuing a
 * duplicate read for a buffer that arrived while it waited. */
int bh_uptodate_or_lock(struct buffer_head *bh);

/* ── getting one ────────────────────────────────────────────────── */

struct buffer_head *__getblk(struct block_device *bdev, sector_t block,
                             unsigned size);
struct buffer_head *__getblk_gfp(struct block_device *bdev, sector_t block,
                                 unsigned size, gfp_t gfp);
struct buffer_head *__bread(struct block_device *bdev, sector_t block,
                            unsigned size);
struct buffer_head *__bread_gfp(struct block_device *bdev, sector_t block,
                                unsigned size, gfp_t gfp);
struct buffer_head *__find_get_block(struct block_device *bdev, sector_t block,
                                     unsigned size);

static inline struct buffer_head *sb_bread(struct super_block *sb,
                                           sector_t block)
{
	return __bread(sb->s_bdev, block, sb->s_blocksize);
}

static inline struct buffer_head *sb_bread_unmovable(struct super_block *sb,
                                                     sector_t block)
{
	return __bread(sb->s_bdev, block, sb->s_blocksize);
}

static inline struct buffer_head *sb_getblk(struct super_block *sb,
                                            sector_t block)
{
	return __getblk(sb->s_bdev, block, sb->s_blocksize);
}

static inline struct buffer_head *sb_getblk_gfp(struct super_block *sb,
                                                sector_t block, gfp_t gfp)
{
	return __getblk_gfp(sb->s_bdev, block, sb->s_blocksize, gfp);
}

static inline struct buffer_head *sb_find_get_block(struct super_block *sb,
                                                    sector_t block)
{
	return __find_get_block(sb->s_bdev, block, sb->s_blocksize);
}

void sb_breadahead(struct super_block *sb, sector_t block);

/* ── reading and writing ────────────────────────────────────────── */

void submit_bh(blk_opf_t opf, struct buffer_head *bh);
/* Read it now and wait. Returns 0 on success and a negative errno on failure —
 * NOT a boolean: a caller that tests it as one treats every error as success. */
int bh_read(struct buffer_head *bh, blk_opf_t op_flags);
int bh_read_nowait(struct buffer_head *bh, blk_opf_t op_flags);
void bh_readahead(struct buffer_head *bh, blk_opf_t op_flags);
void bh_readahead_batch(int nr, struct buffer_head *bhs[], blk_opf_t op_flags);
void __bh_read_batch(int nr, struct buffer_head *bhs[], blk_opf_t op_flags,
                     bool force_lock);

void mark_buffer_dirty(struct buffer_head *bh);
void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode);
void mark_buffer_write_io_error(struct buffer_head *bh);
int sync_dirty_buffer(struct buffer_head *bh);
void write_dirty_buffer(struct buffer_head *bh, blk_opf_t op_flags);
void invalidate_bh_lrus(void);
void invalidate_bh_lrus_cpu(void);

void end_buffer_read_sync(struct buffer_head *bh, int uptodate);
void end_buffer_write_sync(struct buffer_head *bh, int uptodate);
void end_buffer_async_write(struct buffer_head *bh, int uptodate);
/* buffer_io_error is deliberately NOT declared here: ext4 defines a static
 * function of that name in page-io.c, and a non-static declaration in scope
 * makes that a redefinition. Upstream keeps it in fs/buffer.c, unexported. */

/* ── allocation of the heads themselves ─────────────────────────── */

struct buffer_head *alloc_buffer_head(gfp_t gfp_flags);
void free_buffer_head(struct buffer_head *bh);
/* Takes a page, not a folio: 6.6 still spells it that way, and ext4 calls it
 * as `create_empty_buffers(&folio->page, ...)`. */
struct buffer_head *create_empty_buffers(struct folio *folio,
                                         unsigned long blocksize,
                                         unsigned long b_state);
int try_to_free_buffers(struct folio *folio);

/* Point a buffer at a device block. `map_bh` is the whole of what "mapped"
 * means: the block number is known, so the buffer can be read or written. */
static inline void map_bh(struct buffer_head *bh, struct super_block *sb,
                          sector_t block)
{
	set_buffer_mapped(bh);
	bh->b_bdev = sb->s_bdev;
	bh->b_blocknr = block;
	bh->b_size = sb->s_blocksize;
}

/* ── generic address_space operations built on buffers ──────────── */

/* get_block_t is declared in <linux/fs.h>, where upstream keeps it: fs/iomap's
 * internal.h names it while including only that. */

int __block_write_begin(struct folio *folio, loff_t pos, unsigned len,
                        get_block_t *get_block);
int block_read_full_folio(struct folio *folio, get_block_t *get_block);
int block_write_full_page(struct page *page, get_block_t *get_block,
                          struct writeback_control *wbc);
int block_write_begin(struct address_space *mapping, loff_t pos, unsigned len,
                      struct folio **foliop, get_block_t *get_block);
int block_write_end(loff_t pos, unsigned len, unsigned copied,
                    struct folio *folio);
int generic_write_end(const struct kiocb *iocb, struct address_space *mapping,
                      loff_t pos, unsigned len, unsigned copied,
                      struct folio *folio, void *fsdata);
void block_commit_write(struct folio *folio, size_t from, size_t to);
int block_page_mkwrite(struct vm_area_struct *vma, struct vm_fault *vmf,
                       get_block_t get_block);
bool block_dirty_folio(struct address_space *mapping, struct folio *folio);
void block_invalidate_folio(struct folio *folio, size_t offset, size_t length);
int block_truncate_page(struct address_space *mapping, loff_t from,
                        get_block_t *get_block);
sector_t generic_block_bmap(struct address_space *mapping, sector_t block,
                            get_block_t *get_block);
int cont_write_begin(const struct kiocb *iocb, struct address_space *mapping,
                     loff_t pos, unsigned len, struct folio **foliop,
                     void **fsdata, get_block_t *get_block, loff_t *bytes);
int nobh_truncate_page(struct address_space *mapping, loff_t from,
                       get_block_t *get_block);
int sync_mapping_buffers(struct address_space *mapping);
/*
 * fsync for a filesystem whose metadata is in buffers attached to the inode.
 *
 * The `_noflush` form deliberately does NOT issue a device cache flush: ext4
 * calls it from inside a journal commit that will issue one itself, and a
 * second flush per fsync is a measurable cost for no ordering gain.
 */
int generic_buffers_fsync(struct file *file, loff_t start, loff_t end,
                          bool datasync);
int generic_buffers_fsync_noflush(struct file *file, loff_t start, loff_t end,
                                  bool datasync);
void clean_bdev_aliases(struct block_device *bdev, sector_t block,
                        sector_t len);

static inline void clean_bdev_bh_alias(struct buffer_head *bh)
{
	clean_bdev_aliases(bh->b_bdev, bh->b_blocknr, 1);
}

/* block_device_ejected is deliberately NOT declared: ext4 defines a static
 * function of that name in super.c, and a declaration in scope makes that a
 * conflicting definition. */

/* A buffer whose page may not be moved by compaction. b1nix does not compact,
 * so it is the ordinary getblk — the name records the requirement. */
struct buffer_head *getblk_unmovable(struct block_device *bdev, sector_t block,
                                     unsigned size);

struct buffer_head *__find_get_block_nonatomic(struct block_device *bdev,
                                               sector_t block, unsigned size);
static inline struct buffer_head *
sb_find_get_block_nonatomic(struct super_block *sb, sector_t block)
{ return __find_get_block_nonatomic(sb->s_bdev, block, sb->s_blocksize); }
struct buffer_head *bdev_getblk(struct block_device *bdev, sector_t block,
                                unsigned size, gfp_t gfp);
void folio_zero_new_buffers(struct folio *folio, size_t from, size_t to);

#endif
