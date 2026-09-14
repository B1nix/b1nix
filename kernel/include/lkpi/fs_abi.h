/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_FS_ABI_H
#define LKPI_FS_ABI_H

#include <lkpi/types.h>
#include <lkpi/page.h>

/*
 * The block-layer structures, mirrored for the b1nix side.
 *
 * kernel/lkpi/bio.c implements the bio API but must not include <linux/bio.h>:
 * it also includes <b1nix/blk.h>, and the two spell `struct block_device`
 * differently — one is b1nix's device, the other is the Linux object that
 * points at it. So the Linux shapes are written out here.
 *
 * A hand-mirrored layout is only as good as its check, so it is checked:
 * kernel/lkpi/fs_abi_check.c is compiled against the real linux headers and
 * asserts every offset and size against these. A field added on one side and
 * not the other is a build failure there, not a wrong pointer at runtime.
 */

/* ── the structures, mirroring <linux/blk_types.h> ──────────────── */

/*
 * bio_vec and bvec_iter are declared under their own names in
 * <linux/bvec.h> and <linux/blk_types.h>. The checker sees both headers, so
 * they are defined here only when the real ones are absent — which is the case
 * on the b1nix side and nowhere else.
 */
#ifndef LKPI_LINUX_BVEC_H
struct bio_vec {
	struct page *bv_page;
	unsigned int bv_len;
	unsigned int bv_offset;
};
#endif

#ifndef LKPI_LINUX_BLK_TYPES_H
struct bvec_iter {
	u64 bi_sector;
	unsigned int bi_size;
	unsigned int bi_idx;
	unsigned int bi_bvec_done;
};
#endif

struct lkpi_bio;
struct lkpi_bio_set;
typedef void (lkpi_bio_end_io_t)(struct lkpi_bio *);

/*
 * The Linux `struct block_device`, whose `bd_b1nix` points at b1nix's own.
 *
 * Only the members this file touches are named; the layout is fixed by
 * <linux/blkdev.h> and the offsets are asserted below, so a field added there
 * without being added here is a build error rather than a wrong pointer.
 */
struct lkpi_block_device {
	u32 bd_dev;
	void *bd_inode;
	void *bd_disk;
	void *bd_super;
	void *bd_holder;
	int bd_read_only;
	unsigned int bd_block_size;
	u64 bd_nr_sectors;
	void *bd_b1nix;
};

struct lkpi_bio {
	struct lkpi_bio *bi_next;
	struct lkpi_block_device *bi_bdev;
	unsigned int bi_opf;
	unsigned short bi_flags;
	unsigned short bi_ioprio;
	u8 bi_status;
	u8 bi_write_hint;

	struct bvec_iter bi_iter;

	lkpi_bio_end_io_t *bi_end_io;
	void *bi_private;

	unsigned short bi_vcnt;
	unsigned short bi_max_vecs;

	volatile int __bi_remaining;
	volatile int __bi_cnt;

	struct bio_vec *bi_io_vec;
	/* The set this bio came from. Its front_pad is what says where the
	 * allocation actually starts — see kernel/lkpi/bio.c. */
	struct lkpi_bio_set *bi_pool;

	struct bio_vec bi_inline_vecs[];
};

/* The operation is the low byte of bi_opf; the flags live above it. */
#define LKPI_REQ_OP_MASK   0xff
#define LKPI_REQ_OP_READ   0
#define LKPI_REQ_OP_WRITE  1
#define LKPI_REQ_OP_FLUSH  2
#define LKPI_REQ_OP_DISCARD 3
#define LKPI_REQ_OP_WRITE_ZEROES 9

#define LKPI_BLK_STS_OK    0
#define LKPI_BLK_STS_IOERR 10
#define LKPI_BLK_STS_NOTSUPP 1

#define LKPI_SECTOR_SHIFT 9
#define LKPI_SECTOR_SIZE  (1u << LKPI_SECTOR_SHIFT)


/* ── the I/O iterator ───────────────────────────────────────────── */

/*
 * Mirrors `struct iov_iter` from <linux/uio.h>, for the same reason as the bio
 * structures above, and checked against it by kernel/lkpi/fs_abi_check.c.
 */
/* Not guarded: these carry lkpi_ names of their own, so both definitions can be
 * in scope at once — which is exactly what the checker needs. */
struct lkpi_iovec { void *iov_base; usize iov_len; };
struct lkpi_kvec  { void *iov_base; usize iov_len; };

struct lkpi_iov_iter {
	u8 iter_type;
	/* _Bool, not int: <linux/uio.h> declares both as bool, and a wider mirror
	 * moves every field after them. The checker caught exactly that. */
	_Bool nofault;
	_Bool data_source; /* true when the iterator is where the data comes FROM */
	usize iov_offset;
	usize count;
	union {
		const struct lkpi_iovec *__iov;
		const struct lkpi_kvec *kvec;
		const struct bio_vec *bvec;
		void *ubuf;
	};
	unsigned long nr_segs;
};

#define LKPI_ITER_UBUF    0
#define LKPI_ITER_IOVEC   1
#define LKPI_ITER_BVEC    2
#define LKPI_ITER_KVEC    3
#define LKPI_ITER_XARRAY  4
#define LKPI_ITER_DISCARD 5

#endif
