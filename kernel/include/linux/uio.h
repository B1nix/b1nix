/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_UIO_H
#define LKPI_LINUX_UIO_H

#include <linux/types.h>
#include <linux/bvec.h>

/*
 * An iterator over a scatter of buffers, wherever they live.
 *
 * The whole point of `iov_iter` is that a filesystem's read and write paths do
 * not care whether the destination is a user array, a kernel array, a list of
 * pages or a pipe: they call `copy_to_iter` and the iterator knows. That is why
 * it carries a type tag rather than being three interfaces.
 *
 * `data_source` is the one field whose meaning is easy to invert: it is true
 * when the iterator is a SOURCE of data — i.e. a write — and `iov_iter_rw`
 * turns it back into READ/WRITE. Reading it backwards makes a read path copy
 * the wrong way, which is silent.
 */

struct page;
struct pipe_inode_info;

enum iter_type {
	ITER_UBUF,     /* one contiguous user buffer */
	ITER_IOVEC,    /* an array of user buffers */
	ITER_BVEC,     /* an array of (page, offset, len) */
	ITER_KVEC,     /* an array of kernel buffers */
	ITER_XARRAY,
	ITER_DISCARD,
};

struct iovec {
	void *iov_base;
	size_t iov_len;
};

struct kvec {
	void *iov_base;
	size_t iov_len;
};

struct iov_iter {
	u8 iter_type;
	bool nofault;
	bool data_source;   /* true = this iterator is where the data comes FROM */
	size_t iov_offset;  /* bytes already consumed from the current entry */
	size_t count;       /* bytes left in the whole iterator */
	union {
		const struct iovec *__iov;
		const struct kvec *kvec;
		const struct bio_vec *bvec;
		void __user *ubuf;
	};
	unsigned long nr_segs;
};

#define READ  0
#define WRITE 1

static inline enum iter_type iov_iter_type(const struct iov_iter *i)
{
	return (enum iter_type)i->iter_type;
}

static inline bool iter_is_ubuf(const struct iov_iter *i)
{ return iov_iter_type(i) == ITER_UBUF; }
static inline bool iter_is_iovec(const struct iov_iter *i)
{ return iov_iter_type(i) == ITER_IOVEC; }
static inline bool iov_iter_is_bvec(const struct iov_iter *i)
{ return iov_iter_type(i) == ITER_BVEC; }
static inline bool iov_iter_is_kvec(const struct iov_iter *i)
{ return iov_iter_type(i) == ITER_KVEC; }
static inline bool iov_iter_is_discard(const struct iov_iter *i)
{ return iov_iter_type(i) == ITER_DISCARD; }

static inline size_t iov_iter_count(const struct iov_iter *i)
{
	return i->count;
}

/* READ when the iterator is a destination, WRITE when it is a source. */
static inline int iov_iter_rw(const struct iov_iter *i)
{
	return i->data_source ? WRITE : READ;
}

static inline const struct iovec *iter_iov(const struct iov_iter *i)
{
	return i->__iov;
}

void iov_iter_init(struct iov_iter *i, unsigned int direction,
                   const struct iovec *iov, unsigned long nr_segs,
                   size_t count);
void iov_iter_kvec(struct iov_iter *i, unsigned int direction,
                   const struct kvec *kvec, unsigned long nr_segs,
                   size_t count);
void iov_iter_bvec(struct iov_iter *i, unsigned int direction,
                   const struct bio_vec *bvec, unsigned long nr_segs,
                   size_t count);

size_t copy_to_iter(const void *addr, size_t bytes, struct iov_iter *i);
size_t copy_from_iter(void *addr, size_t bytes, struct iov_iter *i);
size_t copy_page_to_iter(struct page *page, size_t offset, size_t bytes,
                         struct iov_iter *i);
size_t copy_page_from_iter(struct page *page, size_t offset, size_t bytes,
                           struct iov_iter *i);
size_t copy_folio_to_iter(struct folio *folio, size_t offset, size_t bytes,
                          struct iov_iter *i);
size_t iov_iter_zero(size_t bytes, struct iov_iter *i);

void iov_iter_advance(struct iov_iter *i, size_t bytes);
/* Put back bytes already consumed. Only valid for bytes this iterator itself
 * consumed — a revert past the start is a bug, not a rewind. */
void iov_iter_revert(struct iov_iter *i, size_t bytes);
void iov_iter_truncate(struct iov_iter *i, u64 count);
void iov_iter_reexpand(struct iov_iter *i, size_t count);
unsigned long iov_iter_alignment(const struct iov_iter *i);
bool iov_iter_is_aligned(const struct iov_iter *i, unsigned addr_mask,
                         unsigned len_mask);
size_t iov_iter_single_seg_count(const struct iov_iter *i);
int iov_iter_npages(const struct iov_iter *i, int maxpages);

ssize_t iov_iter_get_pages2(struct iov_iter *i, struct page **pages,
                            size_t maxsize, unsigned maxpages, size_t *start);
ssize_t iov_iter_extract_pages(struct iov_iter *i, struct page ***pages,
                               size_t maxsize, unsigned int maxpages,
                               unsigned int extract_flags, size_t *offset0);

/*
 * The direction constants, in the spelling that says what the ITERATOR is
 * rather than what the operation is: ITER_DEST means data is written INTO the
 * iterator (a read), ITER_SOURCE means it is read out of it (a write). The
 * older READ/WRITE spelling is the same value and is the one that gets
 * inverted by accident.
 */
#define ITER_DEST   READ
#define ITER_SOURCE WRITE

#define UIO_FASTIOV 8
#define UIO_MAXIOV  1024

static inline bool user_backed_iter(const struct iov_iter *i)
{ return iter_is_ubuf(i) || iter_is_iovec(i); }

ssize_t import_iovec(int type, const struct iovec __user *uvec,
                     unsigned nr_segs, unsigned fast_segs,
                     struct iovec **iovp, struct iov_iter *i);

/*
 * Fault in the pages behind an iterator before taking a lock.
 *
 * A write path holds the inode lock and then copies from userspace; if that
 * copy faults, the fault handler may need the same lock. So the pages are
 * touched first, outside the lock — that is the whole purpose, and skipping it
 * is a deadlock rather than a slowdown.
 */
size_t fault_in_iov_iter_readable(const struct iov_iter *i, size_t bytes);
size_t fault_in_iov_iter_writeable(struct iov_iter *i, size_t bytes);
size_t fault_in_subpage_writeable(char __user *uaddr, size_t size);
size_t fault_in_readable(const char __user *uaddr, size_t size);

size_t copy_page_from_iter_atomic(struct page *page, size_t offset,
                                  size_t bytes, struct iov_iter *i);
size_t copy_folio_from_iter_atomic(struct folio *folio, size_t offset,
                                   size_t bytes, struct iov_iter *i);

/* One user buffer as an iterator. */
static inline void iov_iter_ubuf(struct iov_iter *i, unsigned int direction,
                                 void __user *buf, size_t count)
{
	*i = (struct iov_iter) {
		.iter_type = ITER_UBUF,
		.data_source = direction,
		.ubuf = buf,
		.count = count,
		.nr_segs = 1,
	};
}

#endif
