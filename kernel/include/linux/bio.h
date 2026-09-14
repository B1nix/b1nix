/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BIO_H
#define LKPI_LINUX_BIO_H

#include <linux/types.h>
#include <linux/blk_types.h>
#include <linux/bvec.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/mempool.h>
#include <linux/highmem.h>
/* blkcg_punt_bio_submit and the css helpers: btrfs submits through them from
 * files that include only this header. */
#include <linux/blk-cgroup.h>

/* NOT <linux/blkdev.h>: that header includes THIS one, as upstream has it.
 * What is needed from it here is SECTOR_SHIFT, which lives in
 * <linux/blk_types.h> for exactly this reason, plus two forward declarations. */
struct queue_limits;
struct request_queue;

/*
 * The bio: one I/O request, as a filesystem builds it.
 *
 * The structure itself is in <linux/blk_types.h>; this is the API over it. Two
 * things about the model are worth stating, because getting either wrong
 * produces corruption rather than a failure:
 *
 *   - `bi_iter` is consumed as the I/O proceeds. A submitted bio no longer
 *     describes what it originally asked for. Code that needs the original
 *     saves a copy of the iterator first, and btrfs does exactly that in
 *     several places.
 *
 *   - sectors are 512 bytes here regardless of the device's logical block
 *     size. `bi_iter.bi_sector` is always in 512-byte units.
 *
 * Completion is a callback, not a return value: `bi_end_io` runs when the last
 * byte is done, from whatever context finished it. `submit_bio_wait` is the
 * synchronous wrapper, and it is what almost every filesystem metadata read
 * here actually uses.
 */

struct bio_set {
	/* A pool exists upstream to guarantee forward progress under memory
	 * pressure — a filesystem must be able to write a page out in order to
	 * free memory, so it cannot depend on the allocator to do it. b1nix's
	 * kmalloc panics rather than blocking on OOM, so there is nothing here to
	 * reserve against; the set carries the sizes so a bio allocated from it
	 * still gets the right front pad. */
	unsigned int front_pad;
	unsigned int back_pad;
	int initialised;
};

#define BIOSET_NEED_BVECS  (1 << 0)
#define BIOSET_NEED_RESCUER (1 << 1)
#define BIOSET_PERCPU_CACHE (1 << 2)

int bioset_init(struct bio_set *bs, unsigned int pool_size,
                unsigned int front_pad, int flags);
void bioset_exit(struct bio_set *bs);

/* ── allocation ─────────────────────────────────────────────────── */

struct bio *bio_alloc_bioset(struct block_device *bdev, unsigned short nr_vecs,
                             blk_opf_t opf, gfp_t gfp_mask, struct bio_set *bs);
struct bio *bio_alloc_clone(struct block_device *bdev, struct bio *bio_src,
                            gfp_t gfp, struct bio_set *bs);
struct bio *bio_kmalloc(unsigned short nr_vecs, gfp_t gfp_mask);

static inline struct bio *bio_alloc(struct block_device *bdev,
                                    unsigned short nr_vecs, blk_opf_t opf,
                                    gfp_t gfp_mask)
{
	return bio_alloc_bioset(bdev, nr_vecs, opf, gfp_mask, NULL);
}

/* Initialise a bio the caller owns the storage for — embedded in a larger
 * structure, which is how btrfs allocates most of its. */
void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,
              unsigned short max_vecs, blk_opf_t opf);
void bio_uninit(struct bio *bio);
void bio_reset(struct bio *bio, struct block_device *bdev, blk_opf_t opf);

void bio_put(struct bio *bio);

static inline void bio_get(struct bio *bio)
{
	bio->bi_flags |= (1 << BIO_REFFED);
	atomic_inc(&bio->__bi_cnt);
}

/* ── building ───────────────────────────────────────────────────── */

/* Returns the bytes actually added, which may be zero when the vector is full
 * or the device's segment limit is reached. A caller that ignores the result
 * and assumes the whole page went in writes the wrong length. */
int bio_add_page(struct bio *bio, struct page *page, unsigned int len,
                 unsigned int off);
/* The caller has already checked there is room; adds unconditionally. */
void __bio_add_page(struct bio *bio, struct page *page, unsigned int len,
                    unsigned int off);
bool bio_add_folio(struct bio *bio, struct folio *folio, size_t len,
                   size_t off);
void bio_add_folio_nofail(struct bio *bio, struct folio *folio, size_t len,
                          size_t off);

void bio_set_dev(struct bio *bio, struct block_device *bdev);
void zero_fill_bio(struct bio *bio);
void bio_advance(struct bio *bio, unsigned int nbytes);

/* Chain `bio` onto `parent`: the parent's completion waits for the child. Used
 * wherever one logical I/O is split across devices, which for btrfs is every
 * multi-device profile. */
void bio_chain(struct bio *bio, struct bio *parent);
void bio_inc_remaining(struct bio *bio);
struct bio *bio_split(struct bio *bio, int sectors, gfp_t gfp,
                      struct bio_set *bs);

/* ── submission and completion ──────────────────────────────────── */

void submit_bio(struct bio *bio);
int submit_bio_wait(struct bio *bio);
void bio_endio(struct bio *bio);

static inline void bio_io_error(struct bio *bio)
{
	bio->bi_status = BLK_STS_IOERR;
	bio_endio(bio);
}

/* ── accessors ──────────────────────────────────────────────────── */

static inline enum req_op bio_op(const struct bio *bio)
{
	return bio_op_from_opf(bio->bi_opf);
}

static inline unsigned int bio_sectors(const struct bio *bio)
{
	return bio->bi_iter.bi_size >> SECTOR_SHIFT;
}

static inline sector_t bio_end_sector(const struct bio *bio)
{
	return bio->bi_iter.bi_sector + (sector_t)bio_sectors(bio);
}

static inline bool bio_has_data(struct bio *bio)
{
	return bio && bio->bi_iter.bi_size &&
	       bio_op(bio) != REQ_OP_DISCARD &&
	       bio_op(bio) != REQ_OP_SECURE_ERASE &&
	       bio_op(bio) != REQ_OP_WRITE_ZEROES;
}

static inline bool bio_flagged(const struct bio *bio, unsigned int bit)
{
	return (bio->bi_flags & (1U << bit)) != 0;
}

static inline void bio_set_flag(struct bio *bio, unsigned int bit)
{
	bio->bi_flags |= (1U << bit);
}

static inline void bio_clear_flag(struct bio *bio, unsigned int bit)
{
	bio->bi_flags &= ~(1U << bit);
}

static inline unsigned short bio_max_segs(unsigned short nr_segs)
{
	return nr_segs < BIO_MAX_VECS ? nr_segs : (unsigned short)BIO_MAX_VECS;
}

static inline unsigned int bio_segments(struct bio *bio)
{
	return bio->bi_vcnt;
}

/* ── iteration ──────────────────────────────────────────────────── */

/*
 * Two families, and they are not interchangeable.
 *
 * `_all` walks the vector as it was built — every entry, from index zero —
 * and is only valid on a bio that has not been split, because a split bio's
 * vector is shared with its parent. It is what a completion handler uses to
 * find the pages it must unlock.
 *
 * The iterator forms walk what is LEFT of the I/O from wherever bi_iter has
 * got to, splitting entries at the boundaries the caller asks for. That is what
 * a submission path uses.
 */

static inline struct bio_vec *bio_first_bvec_all(struct bio *bio)
{
	return &bio->bi_io_vec[0];
}

static inline struct page *bio_first_page_all(struct bio *bio)
{
	return bio->bi_io_vec[0].bv_page;
}

static inline struct folio *bio_first_folio_all(struct bio *bio)
{
	return page_folio(bio_first_page_all(bio));
}

static inline struct bio_vec *bio_last_bvec_all(struct bio *bio)
{
	return &bio->bi_io_vec[bio->bi_vcnt - 1];
}

/*
 * Walk every entry of the vector as it was built.
 *
 * The iteration state is a `struct bvec_iter_all`, not an integer, because that
 * is what imported code declares — and upstream needs the struct: a multi-page
 * folio is one bvec that the walk splits into pages, so the state is an index
 * plus a position within the entry. One page per folio here makes the second
 * half constant, and the struct is kept so the call sites compile unchanged.
 *
 * Only valid on a bio that has not been split: a split bio shares its vector
 * with its parent, and walking it from index zero walks the parent's entries.
 */
#define bio_for_each_segment_all(bvl, bio, iter)                               \
	for ((iter).idx = 0, (iter).done = 0,                                      \
	     (bvl) = (bio)->bi_io_vec;                                             \
	     (iter).idx < (bio)->bi_vcnt;                                          \
	     (iter).idx++, (bvl)++)

/*
 * The `_bvec_` form walks the vector ENTRIES rather than the pages inside them,
 * so its iteration state is a plain index — and imported code declares `int i`
 * for it while declaring `struct bvec_iter_all` for the form above. The two
 * therefore cannot share an expansion, which is why upstream has both.
 */
#define bio_for_each_bvec_all(bvl, bio, i)                                     \
	for ((i) = 0, (bvl) = (bio)->bi_io_vec;                                    \
	     (i) < (bio)->bi_vcnt;                                                 \
	     (i)++, (bvl)++)

/*
 * How many bios a set keeps in reserve. Upstream's pool exists to guarantee
 * forward progress under memory pressure; there is no reserve here (see
 * `struct bio_set`), so the constant is the size callers ask for and nothing
 * more.
 */
#define BIO_POOL_SIZE 2
#define BIO_EMPTY_LIST { NULL, NULL }

/*
 * Folio iteration. Every page here is its own folio of one page — see
 * <linux/mm.h> — so this walks the same entries as the page form, presenting
 * each as a folio with its length and offset. `fi.folio`, `fi.offset` and
 * `fi.length` are read directly by imported code, so the member names are
 * upstream's.
 */
struct folio_iter {
	struct folio *folio;
	size_t offset;
	size_t length;
	/* private to the iterator */
	size_t _seg_count;
	int _i;
};

void bio_first_folio(struct folio_iter *fi, struct bio *bio, int i);
void bio_next_folio(struct folio_iter *fi, struct bio *bio);

#define bio_for_each_folio_all(fi, bio)                                        \
	for (bio_first_folio(&(fi), (bio), 0); (fi).folio;                         \
	     bio_next_folio(&(fi), (bio)))

/* One step of the iterator: consume `bytes` from the current entry, moving to
 * the next when it is used up. */
void bio_advance_iter_single(const struct bio *bio, struct bvec_iter *iter,
                             unsigned int bytes);

/* The current entry, clipped to what is left of the I/O. */
struct bio_vec bio_iter_iovec(struct bio *bio, struct bvec_iter iter);

#define bio_iter_len(bio, iter)  (bio_iter_iovec((bio), (iter)).bv_len)
#define bio_iter_offset(bio, iter) (bio_iter_iovec((bio), (iter)).bv_offset)
#define bio_iter_page(bio, iter) (bio_iter_iovec((bio), (iter)).bv_page)

#define __bio_for_each_segment(bvl, bio, iter, start)                          \
	for ((iter) = (start);                                                     \
	     (iter).bi_size && (((bvl) = bio_iter_iovec((bio), (iter))), 1);       \
	     bio_advance_iter_single((bio), &(iter), (bvl).bv_len))

#define bio_for_each_segment(bvl, bio, iter)                                   \
	__bio_for_each_segment(bvl, bio, iter, (bio)->bi_iter)

/* ── lists of bios ──────────────────────────────────────────────── */

/*
 * A singly linked list threaded through bi_next. btrfs collects bios on one
 * before submitting them as a batch, and the whole structure is two pointers
 * because that is all it needs to be.
 */
struct bio_list {
	struct bio *head;
	struct bio *tail;
};

static inline void bio_list_init(struct bio_list *bl)
{
	bl->head = bl->tail = NULL;
}

static inline bool bio_list_empty(const struct bio_list *bl)
{
	return bl->head == NULL;
}

#define bio_list_for_each(bio, bl) \
	for ((bio) = (bl)->head; (bio); (bio) = (bio)->bi_next)

static inline unsigned bio_list_size(const struct bio_list *bl)
{
	unsigned sz = 0;
	struct bio *bio;

	bio_list_for_each(bio, bl)
		sz++;
	return sz;
}

static inline void bio_list_add(struct bio_list *bl, struct bio *bio)
{
	bio->bi_next = NULL;
	if (bl->tail)
		bl->tail->bi_next = bio;
	else
		bl->head = bio;
	bl->tail = bio;
}

static inline void bio_list_add_head(struct bio_list *bl, struct bio *bio)
{
	if (!bl->head)
		bl->tail = bio;
	bio->bi_next = bl->head;
	bl->head = bio;
}

static inline void bio_list_merge(struct bio_list *bl, struct bio_list *bl2)
{
	if (!bl2->head)
		return;
	if (bl->tail)
		bl->tail->bi_next = bl2->head;
	else
		bl->head = bl2->head;
	bl->tail = bl2->tail;
}

static inline struct bio *bio_list_pop(struct bio_list *bl)
{
	struct bio *bio = bl->head;

	if (bio) {
		bl->head = bl->head->bi_next;
		if (!bl->head)
			bl->tail = NULL;
		bio->bi_next = NULL;
	}
	return bio;
}

static inline struct bio *bio_list_get(struct bio_list *bl)
{
	struct bio *bio = bl->head;

	bl->head = bl->tail = NULL;
	return bio;
}

/* ── things that exist only to be absent ────────────────────────── */

/*
 * Page dirtying for direct I/O, cgroup association, and polling.
 *
 * The dirty-tracking pair matters upstream because a direct read writes into
 * user pages from a completion that may run after the process was scheduled
 * away. b1nix's direct I/O path copies through the kernel, so there are no user
 * pages held across a completion and nothing to mark.
 */
static inline void bio_set_pages_dirty(struct bio *bio) { (void)bio; }
static inline void bio_check_pages_dirty(struct bio *bio) { bio_put(bio); }
static inline void bio_release_pages(struct bio *bio, bool mark_dirty)
{ (void)bio; (void)mark_dirty; }
static inline void bio_clone_blkg_association(struct bio *dst, struct bio *src)
{ (void)dst; (void)src; }
static inline void bio_associate_blkg(struct bio *bio) { (void)bio; }
static inline void bio_set_polled(struct bio *bio, struct kiocb *kiocb)
{ (void)bio; (void)kiocb; }

struct iov_iter;
int bio_iov_iter_get_pages(struct bio *bio, struct iov_iter *iter);
int bio_iov_vecs_to_alloc(struct iov_iter *iter, int max_segs);

/* Split a bio at the device's limits, for a caller that submits the remainder
 * itself. Returns NULL when the whole bio fits, which is the common case here:
 * b1nix's block cache takes an arbitrary length. */
struct bio *bio_split_rw(struct bio *bio, const struct queue_limits *lim,
                         unsigned *segs, struct bio_set *bs,
                         unsigned max_bytes);

#endif
