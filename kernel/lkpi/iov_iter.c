/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the I/O iterator.
 *
 * An `iov_iter` is a scatter of buffers with a cursor. Its whole purpose is
 * that a filesystem's read and write paths do not care whether the destination
 * is a user array, a kernel array or a list of pages: they call copy_to_iter
 * and the iterator knows.
 *
 * Two things about the model are easy to get backwards and silent when wrong:
 *
 *   - `data_source` is true when the iterator is where the data comes FROM —
 *     a write. Reading it the other way makes a read path copy the wrong
 *     direction, and the copy still succeeds.
 *   - Every copy ADVANCES the cursor by what it moved, and returns that
 *     amount. A short copy is normal (the iterator ran out); a caller that
 *     assumes the full length wrote past the end of what it was given.
 *
 * On the b1nix side of the boundary: the structures are mirrored in
 * <lkpi/fs_abi.h> and checked against the real ones by
 * kernel/lkpi/fs_abi_check.c.
 */

#include <lkpi/env.h>
#include <lkpi/types.h>
#include <lkpi/page.h>
#include <lkpi/fs_abi.h>
#include <b1nix/klog.h>
#include <string.h>

/* From b1nix's user-access layer. Both return the number of bytes NOT copied,
 * which is why every use below subtracts. */
usize copy_to_user_raw(void *dst, const void *src, usize len);
usize copy_from_user_raw(void *dst, const void *src, usize len);

/* ── walking ────────────────────────────────────────────────────── */

/*
 * Copy `bytes` between `addr` and the iterator, in the given direction, and
 * advance it.
 *
 * One function for both directions because the walk is identical and only the
 * memcpy is mirrored — two copies of this loop is two places for the
 * remaining-bytes arithmetic to drift.
 */
static usize iov_iter_copy(struct lkpi_iov_iter *i, void *addr, usize bytes,
                           int to_iter)
{
	char *p = addr;
	usize done = 0;

	if (bytes > i->count)
		bytes = i->count;

	while (done < bytes) {
		usize chunk;
		char *base;

		switch (i->iter_type) {
		case LKPI_ITER_UBUF: {
			char *ubuf = (char *)i->ubuf + i->iov_offset;

			chunk = bytes - done;
			if (to_iter)
				chunk -= copy_to_user_raw(ubuf, p + done, chunk);
			else
				chunk -= copy_from_user_raw(p + done, ubuf, chunk);
			if (!chunk)
				goto out;   /* a fault: report what was moved */
			i->iov_offset += chunk;
			break;
		}
		case LKPI_ITER_IOVEC: {
			const struct lkpi_iovec *iov;
			char *ubuf;

			if (!i->nr_segs)
				goto out;
			iov = i->__iov;
			ubuf = (char *)iov->iov_base + i->iov_offset;
			chunk = iov->iov_len - i->iov_offset;
			if (chunk > bytes - done)
				chunk = bytes - done;
			if (to_iter)
				chunk -= copy_to_user_raw(ubuf, p + done, chunk);
			else
				chunk -= copy_from_user_raw(p + done, ubuf, chunk);
			if (!chunk)
				goto out;
			i->iov_offset += chunk;
			if (i->iov_offset == iov->iov_len) {
				i->__iov++;
				i->nr_segs--;
				i->iov_offset = 0;
			}
			break;
		}
		case LKPI_ITER_KVEC: {
			const struct lkpi_kvec *kv;

			if (!i->nr_segs)
				goto out;
			kv = i->kvec;
			base = (char *)kv->iov_base + i->iov_offset;
			chunk = kv->iov_len - i->iov_offset;
			if (chunk > bytes - done)
				chunk = bytes - done;
			if (to_iter)
				memcpy(base, p + done, chunk);
			else
				memcpy(p + done, base, chunk);
			i->iov_offset += chunk;
			if (i->iov_offset == kv->iov_len) {
				i->kvec++;
				i->nr_segs--;
				i->iov_offset = 0;
			}
			break;
		}
		case LKPI_ITER_BVEC: {
			const struct bio_vec *bv;

			if (!i->nr_segs)
				goto out;
			bv = i->bvec;
			base = (char *)page_address(bv->bv_page) + bv->bv_offset +
			       i->iov_offset;
			chunk = bv->bv_len - i->iov_offset;
			if (chunk > bytes - done)
				chunk = bytes - done;
			if (to_iter)
				memcpy(base, p + done, chunk);
			else
				memcpy(p + done, base, chunk);
			i->iov_offset += chunk;
			if (i->iov_offset == bv->bv_len) {
				i->bvec++;
				i->nr_segs--;
				i->iov_offset = 0;
			}
			break;
		}
		case LKPI_ITER_DISCARD:
			/* A sink: bytes going to it are accounted and dropped. Reading
			 * FROM one is a caller error, and yields zeros rather than
			 * whatever was in the caller's buffer. */
			chunk = bytes - done;
			if (!to_iter)
				memset(p + done, 0, chunk);
			break;
		default:
			goto out;
		}
		done += chunk;
		i->count -= chunk;
	}
out:
	return done;
}

usize copy_to_iter(const void *addr, usize bytes, struct lkpi_iov_iter *i)
{
	return iov_iter_copy(i, (void *)addr, bytes, 1);
}

usize copy_from_iter(void *addr, usize bytes, struct lkpi_iov_iter *i)
{
	return iov_iter_copy(i, addr, bytes, 0);
}

usize copy_page_to_iter(struct page *page, usize offset, usize bytes,
                        struct lkpi_iov_iter *i)
{
	return copy_to_iter((char *)page_address(page) + offset, bytes, i);
}

usize copy_page_from_iter(struct page *page, usize offset, usize bytes,
                          struct lkpi_iov_iter *i)
{
	return copy_from_iter((char *)page_address(page) + offset, bytes, i);
}

/*
 * The `_atomic` forms.
 *
 * Upstream they must not fault, because the caller holds the page lock or a
 * spinlock — so they copy only what is already resident and report a short
 * result. Here the copy path already reports short results on a fault, so the
 * behaviour is the same one; the name records the caller's constraint.
 */
usize copy_page_from_iter_atomic(struct page *page, usize offset, usize bytes,
                                 struct lkpi_iov_iter *i)
{
	return copy_from_iter((char *)page_address(page) + offset, bytes, i);
}

usize copy_folio_from_iter_atomic(void *folio, usize offset, usize bytes,
                                  struct lkpi_iov_iter *i)
{
	return copy_page_from_iter_atomic((struct page *)folio, offset, bytes, i);
}

usize copy_folio_to_iter(void *folio, usize offset, usize bytes,
                         struct lkpi_iov_iter *i)
{
	return copy_page_to_iter((struct page *)folio, offset, bytes, i);
}

/* Fill the iterator with zeros, as a read of a hole does. */
usize iov_iter_zero(usize bytes, struct lkpi_iov_iter *i)
{
	static const char zeros[512];
	usize done = 0;

	while (done < bytes) {
		usize chunk = bytes - done;

		if (chunk > sizeof(zeros))
			chunk = sizeof(zeros);
		chunk = copy_to_iter(zeros, chunk, i);
		if (!chunk)
			break;
		done += chunk;
	}
	return done;
}

/* ── setting up ─────────────────────────────────────────────────── */

void iov_iter_init(struct lkpi_iov_iter *i, unsigned int direction,
                   const struct lkpi_iovec *iov, unsigned long nr_segs,
                   usize count)
{
	memset(i, 0, sizeof(*i));
	i->iter_type = LKPI_ITER_IOVEC;
	/* WRITE means the iterator is the SOURCE. The direction argument is the
	 * operation, so this is where the two are related, once. */
	i->data_source = (direction == 1);
	i->__iov = iov;
	i->nr_segs = nr_segs;
	i->count = count;
}

void iov_iter_kvec(struct lkpi_iov_iter *i, unsigned int direction,
                   const struct lkpi_kvec *kvec, unsigned long nr_segs,
                   usize count)
{
	memset(i, 0, sizeof(*i));
	i->iter_type = LKPI_ITER_KVEC;
	i->data_source = (direction == 1);
	i->kvec = kvec;
	i->nr_segs = nr_segs;
	i->count = count;
}

void iov_iter_bvec(struct lkpi_iov_iter *i, unsigned int direction,
                   const struct bio_vec *bvec, unsigned long nr_segs,
                   usize count)
{
	memset(i, 0, sizeof(*i));
	i->iter_type = LKPI_ITER_BVEC;
	i->data_source = (direction == 1);
	i->bvec = bvec;
	i->nr_segs = nr_segs;
	i->count = count;
}

/* ── moving the cursor ──────────────────────────────────────────── */

void iov_iter_advance(struct lkpi_iov_iter *i, usize bytes)
{
	/* Advancing without copying: consume the segments the bytes fall in. */
	while (bytes && i->count) {
		usize seg_left;

		switch (i->iter_type) {
		case LKPI_ITER_UBUF:
			seg_left = i->count;
			break;
		case LKPI_ITER_IOVEC:
			seg_left = i->nr_segs ? i->__iov->iov_len - i->iov_offset : 0;
			break;
		case LKPI_ITER_KVEC:
			seg_left = i->nr_segs ? i->kvec->iov_len - i->iov_offset : 0;
			break;
		case LKPI_ITER_BVEC:
			seg_left = i->nr_segs ? i->bvec->bv_len - i->iov_offset : 0;
			break;
		default:
			seg_left = i->count;
			break;
		}
		if (!seg_left)
			break;
		if (bytes < seg_left) {
			i->iov_offset += bytes;
			i->count -= bytes;
			return;
		}
		i->count -= seg_left;
		bytes -= seg_left;
		i->iov_offset = 0;
		switch (i->iter_type) {
		case LKPI_ITER_IOVEC: i->__iov++; i->nr_segs--; break;
		case LKPI_ITER_KVEC:  i->kvec++;  i->nr_segs--; break;
		case LKPI_ITER_BVEC:  i->bvec++;  i->nr_segs--; break;
		default: return;
		}
	}
}

/*
 * Put back bytes this iterator already consumed.
 *
 * Only valid for bytes it consumed itself — a revert past the start is a bug,
 * not a rewind, and is refused rather than wrapping the count.
 */
void iov_iter_revert(struct lkpi_iov_iter *i, usize bytes)
{
	if (!bytes)
		return;
	if (bytes > i->iov_offset) {
		/* Crossing back over a segment boundary would need the segment list
		 * walked backwards, which the forward-only cursor cannot do. No
		 * caller in the imported filesystems reverts further than the
		 * current segment; one that did would be silently wrong here, so it
		 * is reported instead. */
		klog_warn("lkpi iov_iter: revert past the current segment is not "
		          "supported");
		i->count += i->iov_offset;
		i->iov_offset = 0;
		return;
	}
	i->iov_offset -= bytes;
	i->count += bytes;
}

/* Shrink the iterator to `count` bytes. Growing it is what reexpand is for,
 * and only back to what it was — hence the two names. */
void iov_iter_truncate(struct lkpi_iov_iter *i, u64 count)
{
	if (i->count > count)
		i->count = (usize)count;
}

void iov_iter_reexpand(struct lkpi_iov_iter *i, usize count)
{
	i->count = count;
}

/*
 * The alignment shared by every address and length in the iterator.
 *
 * Direct I/O uses it to decide whether a request can go straight to the device
 * or has to be bounced: an OR of everything, so a single misaligned segment
 * makes the whole iterator misaligned — which is the answer that keeps the
 * decision safe.
 */
unsigned long iov_iter_alignment(const struct lkpi_iov_iter *i)
{
	unsigned long res = 0;
	unsigned long n;

	switch (i->iter_type) {
	case LKPI_ITER_UBUF:
		return (unsigned long)i->ubuf | i->count;
	case LKPI_ITER_IOVEC:
		for (n = 0; n < i->nr_segs; n++)
			res |= (unsigned long)i->__iov[n].iov_base | i->__iov[n].iov_len;
		return res;
	case LKPI_ITER_KVEC:
		for (n = 0; n < i->nr_segs; n++)
			res |= (unsigned long)i->kvec[n].iov_base | i->kvec[n].iov_len;
		return res;
	case LKPI_ITER_BVEC:
		for (n = 0; n < i->nr_segs; n++)
			res |= i->bvec[n].bv_offset | i->bvec[n].bv_len;
		return res;
	default:
		return 0;
	}
}

int iov_iter_is_aligned(const struct lkpi_iov_iter *i, unsigned addr_mask,
                        unsigned len_mask)
{
	unsigned long a = iov_iter_alignment(i);

	return (a & (addr_mask | len_mask)) == 0;
}

usize iov_iter_single_seg_count(const struct lkpi_iov_iter *i)
{
	switch (i->iter_type) {
	case LKPI_ITER_IOVEC:
		return i->nr_segs ? i->__iov->iov_len - i->iov_offset : 0;
	case LKPI_ITER_KVEC:
		return i->nr_segs ? i->kvec->iov_len - i->iov_offset : 0;
	case LKPI_ITER_BVEC:
		return i->nr_segs ? i->bvec->bv_len - i->iov_offset : 0;
	default:
		return i->count;
	}
}

int iov_iter_npages(const struct lkpi_iov_iter *i, int maxpages)
{
	usize pages = (i->count + 4095) / 4096;

	return (int)(pages > (usize)maxpages ? (usize)maxpages : pages);
}

/*
 * Fault the iterator's pages in before a lock is taken.
 *
 * A write path holds the inode lock and then copies from userspace; if that
 * copy faults, the fault handler may need the same lock. Touching the pages
 * first, outside the lock, is the whole point — skipping it is a deadlock, not
 * a slowdown.
 *
 * b1nix's user copies fault the page in themselves and cannot re-enter a
 * filesystem lock while doing it (its page fault path does not call into one),
 * so there is nothing to pre-fault and these report success. The day a
 * filesystem backs a mapping, this is where the pre-fault has to become real.
 */
usize fault_in_iov_iter_readable(const struct lkpi_iov_iter *i, usize bytes)
{ (void)i; (void)bytes; return 0; }
usize fault_in_iov_iter_writeable(struct lkpi_iov_iter *i, usize bytes)
{ (void)i; (void)bytes; return 0; }
usize fault_in_subpage_writeable(char *uaddr, usize size)
{ (void)uaddr; (void)size; return 0; }
usize fault_in_readable(const char *uaddr, usize size)
{ (void)uaddr; (void)size; return 0; }

/*
 * Copy a userspace iovec array into the kernel and build an iterator over it.
 *
 * The array itself is in user memory and must be copied before it is walked —
 * a second thread can rewrite it between the check and the use otherwise, and
 * that is a classic way to make a kernel read the wrong address.
 */
isize import_iovec(int type, const struct lkpi_iovec *uvec, unsigned nr_segs,
                   unsigned fast_segs, struct lkpi_iovec **iovp,
                   struct lkpi_iov_iter *i)
{
	void *lkpi_kmalloc(usize size, u32 flags);
	struct lkpi_iovec *iov = *iovp;
	usize total = 0;
	unsigned n;

	if (nr_segs > 1024)
		return -22;   /* -EINVAL */
	if (nr_segs > fast_segs) {
		iov = lkpi_kmalloc(nr_segs * sizeof(*iov), 0);
		if (!iov)
			return -12;  /* -ENOMEM */
		*iovp = iov;
	}
	if (copy_from_user_raw(iov, uvec, nr_segs * sizeof(*iov)))
		return -14;      /* -EFAULT */
	for (n = 0; n < nr_segs; n++)
		total += iov[n].iov_len;
	iov_iter_init(i, (unsigned)type, iov, nr_segs, total);
	return (isize)total;
}
