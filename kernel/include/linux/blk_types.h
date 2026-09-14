/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BLK_TYPES_H
#define LKPI_LINUX_BLK_TYPES_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/bvec.h>
#include <linux/list.h>

/*
 * The block layer's types: what an I/O is, what it is on, and how it ended.
 *
 * Split from <linux/bio.h> exactly as upstream splits it, because the split is
 * load-bearing: a filesystem header that only needs `blk_status_t` or `bio_end_io_t`
 * in a structure member includes this one, and pulling the whole bio API in
 * behind it would make every such header depend on the whole block layer.
 *
 * The numeric values of REQ_OP_* and BLK_STS_* are upstream's. They are not
 * ABI — nothing outside this kernel sees them — but imported code compares and
 * switches on them, and matching upstream means a value read out of a live
 * structure means the same thing in a b1nix log as in a Linux one.
 */

struct bio;
struct block_device;
struct bio_set;

/*
 * A sector is 512 bytes at this interface, whatever the device reports as its
 * logical block size. Defined here rather than in <linux/blkdev.h> because
 * <linux/bio.h> needs it and must not depend on blkdev.h — the dependency runs
 * the other way, blkdev.h includes bio.h, exactly as upstream has it.
 */
#ifndef SECTOR_SHIFT
#define SECTOR_SHIFT 9
#define SECTOR_SIZE  (1 << SECTOR_SHIFT)
#endif

/* ── how an I/O ended ────────────────────────────────────────────── */

typedef u8 blk_status_t;

#define BLK_STS_OK           0
#define BLK_STS_NOTSUPP      ((blk_status_t)1)
#define BLK_STS_TIMEOUT      ((blk_status_t)2)
#define BLK_STS_NOSPC        ((blk_status_t)3)
#define BLK_STS_TRANSPORT    ((blk_status_t)4)
#define BLK_STS_TARGET       ((blk_status_t)5)
#define BLK_STS_RESV_CONFLICT ((blk_status_t)6)
#define BLK_STS_MEDIUM       ((blk_status_t)7)
#define BLK_STS_PROTECTION   ((blk_status_t)8)
#define BLK_STS_RESOURCE     ((blk_status_t)9)
#define BLK_STS_IOERR        ((blk_status_t)10)
#define BLK_STS_AGAIN        ((blk_status_t)12)
#define BLK_STS_DEV_RESOURCE ((blk_status_t)13)
#define BLK_STS_ZONE_RESOURCE ((blk_status_t)14)
#define BLK_STS_ZONE_OPEN_RESOURCE ((blk_status_t)15)
#define BLK_STS_ZONE_ACTIVE_RESOURCE ((blk_status_t)16)
#define BLK_STS_OFFLINE      ((blk_status_t)17)

/* ── what kind of I/O ────────────────────────────────────────────── */

/*
 * The operation lives in the bottom bits of bi_opf and the flags above it.
 * Upstream's layout, kept because imported code builds an opf by ORing the two
 * together and takes it apart again with these same masks.
 */
enum req_op {
	REQ_OP_READ          = 0,
	REQ_OP_WRITE         = 1,
	REQ_OP_FLUSH         = 2,
	REQ_OP_DISCARD       = 3,
	REQ_OP_SECURE_ERASE  = 5,
	REQ_OP_ZONE_APPEND   = 7,
	REQ_OP_WRITE_ZEROES  = 9,
	REQ_OP_ZONE_OPEN     = 10,
	REQ_OP_ZONE_CLOSE    = 11,
	REQ_OP_ZONE_FINISH   = 12,
	REQ_OP_ZONE_RESET    = 13,
	REQ_OP_ZONE_RESET_ALL = 15,
	REQ_OP_DRV_IN        = 34,
	REQ_OP_DRV_OUT       = 35,
	REQ_OP_LAST          = 36,
};

typedef unsigned int blk_opf_t;

#define REQ_OP_BITS 8
#define REQ_OP_MASK ((1u << REQ_OP_BITS) - 1)
#define REQ_FLAG_BITS 24

enum req_flag_bits {
	__REQ_FAILFAST_DEV = REQ_OP_BITS,
	__REQ_FAILFAST_TRANSPORT,
	__REQ_FAILFAST_DRIVER,
	__REQ_SYNC,
	__REQ_META,
	__REQ_PRIO,
	__REQ_NOMERGE,
	__REQ_IDLE,
	__REQ_INTEGRITY,
	__REQ_FUA,
	__REQ_PREFLUSH,
	__REQ_RAHEAD,
	__REQ_BACKGROUND,
	__REQ_NOWAIT,
	__REQ_POLLED,
	__REQ_ALLOC_CACHE,
	__REQ_SWAP,
	__REQ_DRV,
	__REQ_FS_PRIVATE,
	__REQ_NR_BITS,
};

#define REQ_FAILFAST_DEV       (1ULL << __REQ_FAILFAST_DEV)
#define REQ_FAILFAST_TRANSPORT (1ULL << __REQ_FAILFAST_TRANSPORT)
#define REQ_FAILFAST_DRIVER    (1ULL << __REQ_FAILFAST_DRIVER)
#define REQ_SYNC               (1ULL << __REQ_SYNC)
#define REQ_META               (1ULL << __REQ_META)
#define REQ_PRIO               (1ULL << __REQ_PRIO)
#define REQ_NOMERGE            (1ULL << __REQ_NOMERGE)
#define REQ_IDLE               (1ULL << __REQ_IDLE)
#define REQ_INTEGRITY          (1ULL << __REQ_INTEGRITY)
#define REQ_FUA                (1ULL << __REQ_FUA)
#define REQ_PREFLUSH           (1ULL << __REQ_PREFLUSH)
#define REQ_RAHEAD             (1ULL << __REQ_RAHEAD)
#define REQ_BACKGROUND         (1ULL << __REQ_BACKGROUND)
#define REQ_NOWAIT             (1ULL << __REQ_NOWAIT)
#define REQ_POLLED             (1ULL << __REQ_POLLED)
#define REQ_ALLOC_CACHE        (1ULL << __REQ_ALLOC_CACHE)
#define REQ_SWAP               (1ULL << __REQ_SWAP)
#define REQ_DRV                (1ULL << __REQ_DRV)
#define REQ_FS_PRIVATE         (1ULL << __REQ_FS_PRIVATE)

/* btrfs names its own bit off REQ_DRV; the definition is upstream's and lives
 * in btrfs's own headers, but the base bit has to be here. */
#define REQ_FAILFAST_MASK \
	(REQ_FAILFAST_DEV | REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER)

static inline enum req_op bio_op_from_opf(blk_opf_t opf)
{
	return (enum req_op)(opf & REQ_OP_MASK);
}

static inline int op_is_write(blk_opf_t op)
{
	return (op & 1) != 0;
}

static inline int op_is_flush(blk_opf_t op)
{
	return (op & (REQ_FUA | REQ_PREFLUSH)) != 0;
}

static inline int op_is_sync(blk_opf_t op)
{
	enum req_op o = bio_op_from_opf(op);
	return o == REQ_OP_READ || (op & (REQ_SYNC | REQ_FUA | REQ_PREFLUSH));
}

static inline int op_is_discard(blk_opf_t op)
{
	enum req_op o = bio_op_from_opf(op);
	return o == REQ_OP_DISCARD || o == REQ_OP_SECURE_ERASE;
}

static inline int op_is_zone_mgmt(enum req_op op)
{
	switch (op) {
	case REQ_OP_ZONE_RESET:
	case REQ_OP_ZONE_RESET_ALL:
	case REQ_OP_ZONE_OPEN:
	case REQ_OP_ZONE_CLOSE:
	case REQ_OP_ZONE_FINISH:
		return 1;
	default:
		return 0;
	}
}

/* ── the bio ─────────────────────────────────────────────────────── */

typedef void (bio_end_io_t)(struct bio *);

/*
 * Where a bio has got to.
 *
 * `bi_sector` advances and `bi_size` shrinks as the I/O completes, which is why
 * a bio that has been submitted can no longer describe what it originally
 * asked for — imported code that needs the original saves a copy of this
 * struct before submitting, and several places in btrfs do exactly that.
 */
struct bvec_iter {
	sector_t bi_sector;      /* in 512-byte units, always */
	unsigned int bi_size;    /* bytes left to transfer */
	unsigned int bi_idx;     /* current index into bi_io_vec */
	unsigned int bi_bvec_done; /* bytes done in the current bvec */
};

struct bvec_iter_all {
	struct bio_vec bv;
	int idx;
	unsigned done;
};

enum {
	BIO_NO_PAGE_REF,
	BIO_CLONED,
	BIO_BOUNCED,
	BIO_QUIET,
	BIO_CHAIN,
	BIO_REFFED,
	BIO_BPS_THROTTLED,
	BIO_TRACE_COMPLETION,
	BIO_CGROUP_ACCT,
	BIO_QOS_THROTTLED,
	BIO_QOS_MERGED,
	BIO_REMAPPED,
	BIO_ZONE_WRITE_LOCKED,
	BIO_FLAG_LAST,
};

struct bio {
	struct bio *bi_next;
	struct block_device *bi_bdev;
	blk_opf_t bi_opf;
	unsigned short bi_flags;
	unsigned short bi_ioprio;
	blk_status_t bi_status;

	struct bvec_iter bi_iter;

	bio_end_io_t *bi_end_io;
	void *bi_private;

	unsigned short bi_vcnt;     /* bio_vec entries in use */
	unsigned short bi_max_vecs; /* entries bi_io_vec has room for */

	atomic_t __bi_remaining;    /* chained children still outstanding */
	atomic_t __bi_cnt;          /* references */

	struct bio_vec *bi_io_vec;
	struct bio_set *bi_pool;

	/*
	 * The inline vector. A bio allocated for a handful of pages — which is
	 * most of them — gets its vector from here and never touches the
	 * allocator for it. The array is sized at allocation time and this
	 * member is the start of it, so `struct bio` is never allocated by
	 * itself with sizeof().
	 */
	struct bio_vec bi_inline_vecs[];
};

#define BIO_MAX_VECS 256U

/* Zone models a device can report. b1nix has no zoned devices, so everything
 * answers "none" — but btrfs asks, and the answer has to be a real value. */
enum blk_zoned_model {
	BLK_ZONED_NONE = 0,
	BLK_ZONED_HA,
	BLK_ZONED_HM,
};

#endif
