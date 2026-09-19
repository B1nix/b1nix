/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The layout check between the two sides of the boundary.
 *
 * kernel/lkpi/bio.c implements the block-layer API on the b1nix side, where it
 * cannot include <linux/bio.h> — it also includes <b1nix/blk.h>, and the two
 * spell `struct block_device` differently. So it works through the mirrored
 * shapes in <lkpi/fs_abi.h>.
 *
 * This file is the only place both definitions are in scope at once. It
 * contains no code: every statement is a compile-time assertion that the
 * mirror still matches the original. A field added to one and not the other
 * fails HERE, loudly, rather than becoming a pointer into the wrong word in a
 * filesystem three layers up.
 *
 * It is compiled with the imported flags (see the Makefile), which is what puts
 * the real Linux headers in front of it.
 */

#include <linux/blk_types.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/uio.h>
#include <lkpi/fs_abi.h>

#define SAME_SIZE(a, b) \
	_Static_assert(sizeof(a) == sizeof(b), #a " and " #b " must be the same size")
#define SAME_OFF(ta, ma, tb, mb) \
	_Static_assert(offsetof(ta, ma) == offsetof(tb, mb), \
	               #ta "." #ma " and " #tb "." #mb " must be at the same offset")

/* struct bio_vec is shared by name, so it is the same type — assert it anyway,
 * because that is what makes the rest of the comparison meaningful. */
SAME_SIZE(struct bio_vec, struct bio_vec);

/* The iterator. Every member is read and written by both sides. */
SAME_SIZE(struct bvec_iter, struct bvec_iter);

/* The bio. */
SAME_SIZE(struct bio, struct lkpi_bio);
SAME_OFF(struct bio, bi_next, struct lkpi_bio, bi_next);
SAME_OFF(struct bio, bi_bdev, struct lkpi_bio, bi_bdev);
SAME_OFF(struct bio, bi_opf, struct lkpi_bio, bi_opf);
SAME_OFF(struct bio, bi_flags, struct lkpi_bio, bi_flags);
SAME_OFF(struct bio, bi_ioprio, struct lkpi_bio, bi_ioprio);
SAME_OFF(struct bio, bi_status, struct lkpi_bio, bi_status);
SAME_OFF(struct bio, bi_iter, struct lkpi_bio, bi_iter);
SAME_OFF(struct bio, bi_end_io, struct lkpi_bio, bi_end_io);
SAME_OFF(struct bio, bi_private, struct lkpi_bio, bi_private);
SAME_OFF(struct bio, bi_vcnt, struct lkpi_bio, bi_vcnt);
SAME_OFF(struct bio, bi_max_vecs, struct lkpi_bio, bi_max_vecs);
SAME_OFF(struct bio, __bi_remaining, struct lkpi_bio, __bi_remaining);
SAME_OFF(struct bio, __bi_cnt, struct lkpi_bio, __bi_cnt);
SAME_OFF(struct bio, bi_io_vec, struct lkpi_bio, bi_io_vec);
SAME_OFF(struct bio, bi_pool, struct lkpi_bio, bi_pool);
SAME_OFF(struct bio, bi_inline_vecs, struct lkpi_bio, bi_inline_vecs);

/* The block device, as far as the b1nix side reaches into it. Only the members
 * it touches are mirrored, so only those are asserted — the rest of the real
 * structure may grow freely, which is why bd_b1nix is last on both sides. */
SAME_OFF(struct block_device, bd_dev, struct lkpi_block_device, bd_dev);
SAME_OFF(struct block_device, bd_inode, struct lkpi_block_device, bd_inode);
SAME_OFF(struct block_device, bd_disk, struct lkpi_block_device, bd_disk);
SAME_OFF(struct block_device, bd_super, struct lkpi_block_device, bd_super);
SAME_OFF(struct block_device, bd_holder, struct lkpi_block_device, bd_holder);
SAME_OFF(struct block_device, bd_read_only, struct lkpi_block_device,
         bd_read_only);
SAME_OFF(struct block_device, bd_block_size, struct lkpi_block_device,
         bd_block_size);
SAME_OFF(struct block_device, bd_nr_sectors, struct lkpi_block_device,
         bd_nr_sectors);
SAME_OFF(struct block_device, bd_b1nix, struct lkpi_block_device, bd_b1nix);

/* The timestamp, which kernel/lkpi/fs_util.c mirrors. */
struct lkpi_timespec64 { long long tv_sec; long tv_nsec; };
SAME_SIZE(struct timespec64, struct lkpi_timespec64);
SAME_OFF(struct timespec64, tv_sec, struct lkpi_timespec64, tv_sec);
SAME_OFF(struct timespec64, tv_nsec, struct lkpi_timespec64, tv_nsec);

/* The I/O iterator. */
SAME_SIZE(struct iov_iter, struct lkpi_iov_iter);
SAME_OFF(struct iov_iter, iter_type, struct lkpi_iov_iter, iter_type);
SAME_OFF(struct iov_iter, nofault, struct lkpi_iov_iter, nofault);
SAME_OFF(struct iov_iter, data_source, struct lkpi_iov_iter, data_source);
SAME_OFF(struct iov_iter, iov_offset, struct lkpi_iov_iter, iov_offset);
SAME_OFF(struct iov_iter, count, struct lkpi_iov_iter, count);
SAME_OFF(struct iov_iter, nr_segs, struct lkpi_iov_iter, nr_segs);
SAME_SIZE(struct iovec, struct lkpi_iovec);
SAME_SIZE(struct kvec, struct lkpi_kvec);
_Static_assert(LKPI_ITER_UBUF == ITER_UBUF, "ITER_UBUF");
_Static_assert(LKPI_ITER_IOVEC == ITER_IOVEC, "ITER_IOVEC");
_Static_assert(LKPI_ITER_BVEC == ITER_BVEC, "ITER_BVEC");
_Static_assert(LKPI_ITER_KVEC == ITER_KVEC, "ITER_KVEC");
_Static_assert(LKPI_ITER_DISCARD == ITER_DISCARD, "ITER_DISCARD");

/* The operation and status values the b1nix side hard-codes, against the
 * enumerations they mirror. */
_Static_assert(LKPI_REQ_OP_READ == REQ_OP_READ, "REQ_OP_READ");
_Static_assert(LKPI_REQ_OP_WRITE == REQ_OP_WRITE, "REQ_OP_WRITE");
_Static_assert(LKPI_REQ_OP_FLUSH == REQ_OP_FLUSH, "REQ_OP_FLUSH");
_Static_assert(LKPI_REQ_OP_DISCARD == REQ_OP_DISCARD, "REQ_OP_DISCARD");
_Static_assert(LKPI_REQ_FUA == REQ_FUA, "REQ_FUA");
_Static_assert(LKPI_REQ_PREFLUSH == REQ_PREFLUSH, "REQ_PREFLUSH");
_Static_assert(LKPI_REQ_OP_WRITE_ZEROES == REQ_OP_WRITE_ZEROES,
               "REQ_OP_WRITE_ZEROES");
_Static_assert(LKPI_REQ_OP_MASK == REQ_OP_MASK, "REQ_OP_MASK");
_Static_assert(LKPI_BLK_STS_OK == BLK_STS_OK, "BLK_STS_OK");
_Static_assert(LKPI_BLK_STS_IOERR == BLK_STS_IOERR, "BLK_STS_IOERR");
_Static_assert(LKPI_BLK_STS_NOTSUPP == BLK_STS_NOTSUPP, "BLK_STS_NOTSUPP");
_Static_assert(LKPI_SECTOR_SHIFT == SECTOR_SHIFT, "SECTOR_SHIFT");
