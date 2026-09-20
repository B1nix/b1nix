/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zram — a block device whose blocks live compressed in RAM.
 *
 * The point of it is swap. A machine with no disk to swap to, or one whose
 * disk is far slower than its CPU, can still have a place to put pages it is
 * not using: compress them and keep them in memory. Two to three pages fit in
 * the space of one, so the working set that fits in RAM grows by about that
 * much, and the price is CPU time on the eviction path instead of I/O.
 *
 * zswap (kernel/mm/swap.c) already compresses pages in RAM, and the two are
 * not the same thing even though both use LZ4. zswap sits IN FRONT of a real
 * swap device: a page that does not compress well goes to the disk behind it,
 * and a full pool falls through. zram IS the device — there is nothing behind
 * it, so it must store every page it is given, compressed or not, and it is
 * configured and measured by userspace the way a disk is. A machine can run
 * either, or both, and `swapon /dev/zram0` is all it takes to choose this one.
 *
 * Interface: Linux's, because that is what userspace drives. The disk is sized
 * by writing /sys/block/zram0/disksize, released by writing
 * /sys/block/zram0/reset, and measured by reading mm_stat — which is what
 * zramctl, systemd's zram-generator and Debian's zram-tools all do. Nothing
 * here is configured from the kernel command line, for the same reason Linux
 * does not: the size that makes sense is a policy decision about a machine.
 *
 * Storage: one entry per 4 KiB page of the device, each holding a kmalloc'd
 * LZ4 blob. A page of zeroes — which is most of a fresh swap device, and every
 * page a program allocates and never writes — is stored as nothing at all.
 * An incompressible page is stored raw, which costs a page plus the entry, and
 * is why a zram device can never be counted on to hold more than its size.
 */
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/lz4.h>
#include <b1nix/mm.h>
#include <b1nix/spinlock.h>
#include <b1nix/sysfs_attr.h>
#include <stdio.h>
#include <string.h>

/* One device. Linux defaults to one too (num_devices=1); a second is added by
 * userspace through /sys/class/zram-control, which nothing here asks for. */
#define ZRAM_DEVICES 1

/* A page that did not compress is kept raw, and marked by a size no
 * compressed blob can have (a blob is bounded by LZ4_COMPRESS_BOUND). */
#define ZRAM_RAW 0xFFFFu

/* A page of the device. Three states, and the cheapest one is the common one:
 *   data == 0, size == 0   nothing has ever been written here
 *   data == 0, size == 1   zeroes were written here; they cost nothing to keep
 *   data != 0              a blob of `size` bytes, or a raw page (ZRAM_RAW)
 * The first two read back identically; they are distinguished only so that
 * mm_stat's same_pages counts what userspace stored rather than what it never
 * touched. */
struct zram_page {
	u8 *data;
	u16 size;
};

#define ZRAM_ZERO 1u   /* size value for a stored page of zeroes */

struct zram {
	struct block_device bdev;
	char name[8];
	struct sysfs_dir *sysfs;
	spinlock_t lock;

	u64 disksize;            /* bytes, as userspace set it */
	usize npages;
	struct zram_page *pages;
	u16 *scratch;            /* LZ4 hash table, used under the lock */
	u8 *bounce;              /* one decompressed page, used under the lock */

	/* mm_stat, in the order Linux prints it. */
	u64 orig_bytes;          /* what userspace has stored, uncompressed */
	u64 compr_bytes;         /* what it costs now */
	u64 mem_used_max;
	u64 same_pages;          /* pages of zeroes, stored as nothing */
	u64 huge_pages;          /* pages kept raw because they did not compress */

	int registered;
};

static struct zram g_zram[ZRAM_DEVICES];

/* ── the store ───────────────────────────────────────────────────────────── */

/* Forget one page's storage and RETURN its blob for the caller to free after
 * dropping the lock. Freeing it here would be a kfree under z->lock, which is
 * the deadlock described above zram_page_write. Caller holds the lock. */
static u8 *zram_page_drop(struct zram *z, usize idx)
{
	struct zram_page *pg = &z->pages[idx];
	u8 *old = 0;

	if (!pg->data && pg->size == 0)
		return 0; /* never written: there is nothing to account for */

	if (pg->data) {
		z->compr_bytes -= (pg->size == ZRAM_RAW) ? PAGE_SIZE : pg->size;
		if (pg->size == ZRAM_RAW && z->huge_pages)
			z->huge_pages--;
		old = pg->data;
		pg->data = 0;
	} else if (z->same_pages) {
		z->same_pages--;
	}
	pg->size = 0;
	if (z->orig_bytes >= PAGE_SIZE)
		z->orig_bytes -= PAGE_SIZE;
	return old;
}

/* Decompress page `idx` into `out` (PAGE_SIZE bytes). A page nothing has
 * written reads back as zeroes, which is what a fresh disk does. Caller holds
 * the lock. Returns 0, or -1 if the stored blob does not decode -- which would
 * be memory corruption, not bad input, so it is reported rather than hidden. */
static int zram_page_read(struct zram *z, usize idx, u8 *out)
{
	struct zram_page *pg = &z->pages[idx];

	if (!pg->data) {
		memset(out, 0, PAGE_SIZE);
		return 0;
	}
	if (pg->size == ZRAM_RAW) {
		memcpy(out, pg->data, PAGE_SIZE);
		return 0;
	}
	if (lz4_decompress(pg->data, (int)pg->size, out, PAGE_SIZE) != 0) {
		console_write("zram: stored page did not decompress\n");
		return -1;
	}
	return 0;
}

/* Is every byte zero? Checked eight bytes at a time: a swap device is mostly
 * zeroes, and not storing them at all is the largest single saving zram has. */
static int zram_all_zero(const u8 *p)
{
	const u64 *q = (const u64 *)(const void *)p;

	for (usize i = 0; i < PAGE_SIZE / sizeof(u64); i++)
		if (q[i])
			return 0;
	return 1;
}

/*
 * Replace page `idx` with the PAGE_SIZE bytes at `in`.
 *
 * NOTHING HERE MAY ALLOCATE WHILE z->lock IS HELD, and that is the whole shape
 * of this function. The lock order runs the other way round: the kernel heap's
 * lock is taken first, and growing the heap can reclaim, and reclaiming writes
 * a page to swap, and swap may be this device -- so a kmalloc under z->lock
 * closes a cycle with a kmalloc that is already inside the heap lock. That
 * deadlock is not theoretical; it is a spinlock-lockup panic in a btrfs
 * transaction thread, thirty seconds after swap moves onto zram.
 *
 * So: allocate first, compress under the lock into the buffer that is already
 * ours, install it, and free the old blob after letting go. The compressed
 * form is then copied into a buffer of its true size in a second step, because
 * keeping every page at the worst-case bound would make a compressed device
 * cost more than an uncompressed one -- and that second step, too, allocates
 * outside the lock and installs inside it.
 *
 * Returns 0, or -1 when there is no memory for the blob, which the block layer
 * sees as a failed write exactly as a full disk would be.
 */
static int zram_page_write(struct zram *z, usize idx, const u8 *in)
{
	int cap = LZ4_COMPRESS_BOUND(PAGE_SIZE);
	u8 *pre = kmalloc((usize)cap);   /* the write buffer, allocated first */

	if (!pre)
		return -1;

	u64 flags;
	u8 *old = 0;
	u16 stored_size;

	spin_lock_irqsave(&z->lock, &flags);
	if (!z->pages || idx >= z->npages) { /* reset raced with this write */
		spin_unlock_irqrestore(&z->lock, flags);
		kfree(pre);
		return -1;
	}
	struct zram_page *pg = &z->pages[idx];

	if (zram_all_zero(in)) {
		old = zram_page_drop(z, idx);
		pg->size = (u16)ZRAM_ZERO;
		z->same_pages++;
		z->orig_bytes += PAGE_SIZE;
		spin_unlock_irqrestore(&z->lock, flags);
		kfree(old);
		kfree(pre);
		return 0;
	}

	int csize = lz4_compress(in, PAGE_SIZE, pre, cap, z->scratch);

	if (csize > 0 && csize < (int)PAGE_SIZE) {
		stored_size = (u16)csize;
	} else {
		/* Incompressible: storing the compressed form would cost MORE than
		 * the page, so the page itself is the smaller answer. */
		memcpy(pre, in, PAGE_SIZE);
		stored_size = ZRAM_RAW;
	}
	old = zram_page_drop(z, idx);
	pg->data = pre;
	pg->size = stored_size;
	z->compr_bytes += (stored_size == ZRAM_RAW) ? PAGE_SIZE : stored_size;
	if (stored_size == ZRAM_RAW)
		z->huge_pages++;
	z->orig_bytes += PAGE_SIZE;
	if (z->compr_bytes > z->mem_used_max)
		z->mem_used_max = z->compr_bytes;
	spin_unlock_irqrestore(&z->lock, flags);
	kfree(old);

	/* The blob is holding a whole page-and-a-bit for what may be a few hundred
	 * bytes. Hand the slack back: allocate the true size with no lock held,
	 * then install it only if the page is still the one we just wrote. */
	if (stored_size != ZRAM_RAW && stored_size < cap / 2) {
		u8 *fit = kmalloc(stored_size);

		if (fit) {
			spin_lock_irqsave(&z->lock, &flags);
			if (z->pages && idx < z->npages && z->pages[idx].data == pre &&
			    z->pages[idx].size == stored_size) {
				memcpy(fit, pre, stored_size);
				z->pages[idx].data = fit;
				fit = 0;    /* installed */
			}
			spin_unlock_irqrestore(&z->lock, flags);
			if (fit)
				kfree(fit); /* somebody rewrote the page first */
			else
				kfree(pre);
		}
	}
	return 0;
}

/* ── the block device ────────────────────────────────────────────────────── */

#define ZRAM_SECTORS_PER_PAGE (PAGE_SIZE / 512)

/* Copy `len` bytes from page `idx`, starting at `off`, into `out`. Takes the
 * lock itself and allocates nothing. */
static int zram_page_copy_out(struct zram *z, usize idx, usize off, usize len,
                              u8 *out)
{
	u64 flags;

	spin_lock_irqsave(&z->lock, &flags);
	if (!z->pages || idx >= z->npages) {
		spin_unlock_irqrestore(&z->lock, flags);
		return -1;
	}
	int rc = zram_page_read(z, idx, z->bounce);

	if (rc == 0)
		memcpy(out, z->bounce + off, len);
	spin_unlock_irqrestore(&z->lock, flags);
	return rc;
}

static int zram_rw(struct block_device *dev, u64 lba, u32 count, void *buffer,
                   int write)
{
	struct zram *z = dev ? dev->priv : 0;
	u8 *user = buffer;

	if (!z || !buffer)
		return -1;
	if (!z->pages || !z->npages)
		return -1; /* no disksize yet: there is no disk to talk to */
	if (lba + count > dev->block_count)
		return -1;

	/* The scratch page for a partial write, allocated before any lock is
	 * taken and only when the request needs one. Writes that do not cover a
	 * whole page have to read the page first, and the read has to land
	 * somewhere that is not the shared bounce buffer, because the write that
	 * follows needs the lock released in between.
	 *
	 * Two partial writes to the same page can then race, and the later one
	 * wins whole. Every write to this device arrives through the block cache,
	 * which serialises the writers of one block, so that race needs two
	 * callers going behind the cache at once -- and the honest note is here
	 * rather than a lock that would have to be held across an allocation. */
	u8 *rmw = 0;
	int rc = 0;

	for (u32 s = 0; s < count; s++) {
		u64 sector = lba + s;
		usize idx = (usize)(sector / ZRAM_SECTORS_PER_PAGE);
		usize off = (usize)(sector % ZRAM_SECTORS_PER_PAGE) * 512;

		if (idx >= z->npages) {
			rc = -1;
			break;
		}
		if (!write) {
			if (zram_page_copy_out(z, idx, off, 512, user + (usize)s * 512) != 0) {
				rc = -1;
				break;
			}
			continue;
		}
		/* A whole page written in one go needs no read first: every byte of
		 * it is about to be replaced. That is the swap path -- swap always
		 * moves a full page -- so it is the one worth taking. */
		if (off == 0 && count - s >= ZRAM_SECTORS_PER_PAGE) {
			if (zram_page_write(z, idx, user + (usize)s * 512) != 0) {
				rc = -1;
				break;
			}
			s += ZRAM_SECTORS_PER_PAGE - 1;
			continue;
		}
		if (!rmw) {
			rmw = kmalloc(PAGE_SIZE);
			if (!rmw) {
				rc = -1;
				break;
			}
		}
		if (zram_page_copy_out(z, idx, 0, PAGE_SIZE, rmw) != 0) {
			rc = -1;
			break;
		}
		memcpy(rmw + off, user + (usize)s * 512, 512);
		if (zram_page_write(z, idx, rmw) != 0) {
			rc = -1;
			break;
		}
	}
	kfree(rmw);
	return rc;
}

static int zram_read_blocks(struct block_device *dev, u64 lba, u32 count,
                            void *buffer)
{
	return zram_rw(dev, lba, count, buffer, 0);
}

static int zram_write_blocks(struct block_device *dev, u64 lba, u32 count,
                             const void *buffer)
{
	return zram_rw(dev, lba, count, (void *)buffer, 1);
}

/* ── size and reset ──────────────────────────────────────────────────────── */

/* Release every page and the tables. Caller holds no lock. */
static void zram_teardown(struct zram *z)
{
	u64 flags;
	struct zram_page *pages;
	usize npages;
	u16 *scratch;
	u8 *bounce;

	spin_lock_irqsave(&z->lock, &flags);
	pages = z->pages;
	npages = z->npages;
	scratch = z->scratch;
	bounce = z->bounce;
	z->pages = 0;
	z->npages = 0;
	z->scratch = 0;
	z->bounce = 0;
	z->disksize = 0;
	z->orig_bytes = 0;
	z->compr_bytes = 0;
	z->same_pages = 0;
	z->huge_pages = 0;
	z->mem_used_max = 0;
	z->bdev.block_count = 0;
	spin_unlock_irqrestore(&z->lock, flags);

	/* Freeing is done outside the lock: it is a lot of kfree calls, and
	 * nothing can reach the pages any more -- the device reports zero
	 * blocks and every entry point checks z->pages first. */
	for (usize i = 0; i < npages; i++)
		if (pages && pages[i].data)
			kfree(pages[i].data);
	kfree(pages);
	kfree(scratch);
	kfree(bounce);
}

/* Size the device. Returns 0, or a negative errno. */
static int zram_set_disksize(struct zram *z, u64 bytes)
{
	if (!bytes)
		return -EINVAL;
	if (z->pages)
		return -EBUSY; /* Linux too: reset it before resizing it */

	u64 npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

	/* The entry table alone is 16 bytes per page of device. A disksize
	 * larger than RAM is not refused -- a compressed device may legitimately
	 * be told to hold more than it could store -- but one whose bookkeeping
	 * would not fit is, because that memory is spent whether or not anything
	 * is ever written. */
	u64 table_bytes = npages * sizeof(struct zram_page);

	if (table_bytes > pmm_total_usable_memory() / 8)
		return -ENOMEM;

	struct zram_page *pages = kzalloc((usize)table_bytes);
	u16 *scratch = kmalloc(LZ4_HASH_ENTRIES * sizeof(u16));
	u8 *bounce = kmalloc(PAGE_SIZE);

	if (!pages || !scratch || !bounce) {
		kfree(pages);
		kfree(scratch);
		kfree(bounce);
		return -ENOMEM;
	}

	u64 flags;

	spin_lock_irqsave(&z->lock, &flags);
	z->pages = pages;
	z->npages = (usize)npages;
	z->scratch = scratch;
	z->bounce = bounce;
	z->disksize = npages * PAGE_SIZE;
	z->bdev.block_count = npages * ZRAM_SECTORS_PER_PAGE;
	spin_unlock_irqrestore(&z->lock, flags);

	console_write("zram: ");
	console_write(z->name);
	console_write(" sized to ");
	console_write_dec(z->disksize / (1024 * 1024));
	console_write(" MiB\n");
	return 0;
}

/* ── /sys/block/zramN ────────────────────────────────────────────────────── */

/* Linux accepts a suffix on disksize, and so does everything that writes it. */
static u64 zram_parse_size(const char *buf, usize len)
{
	u64 v = 0;
	usize i = 0;

	while (i < len && buf[i] >= '0' && buf[i] <= '9') {
		u64 next = v * 10 + (u64)(buf[i] - '0');

		if (next < v)
			return 0; /* overflowed: not a size anything means */
		v = next;
		i++;
	}
	if (i == 0)
		return 0;
	if (i < len) {
		char c = buf[i];

		if (c == 'K' || c == 'k')
			v *= 1024ull;
		else if (c == 'M' || c == 'm')
			v *= 1024ull * 1024;
		else if (c == 'G' || c == 'g')
			v *= 1024ull * 1024 * 1024;
	}
	return v;
}

static isize zram_show_u64(char *buf, usize cap, u64 v)
{
	char tmp[24];
	usize n = 0;

	if (!v) {
		tmp[n++] = '0';
	} else {
		char rev[24];
		usize r = 0;

		while (v && r < sizeof(rev)) {
			rev[r++] = (char)('0' + (v % 10));
			v /= 10;
		}
		while (r)
			tmp[n++] = rev[--r];
	}
	tmp[n++] = '\n';
	if (n > cap)
		return -EINVAL;
	memcpy(buf, tmp, n);
	return (isize)n;
}

static isize zram_disksize_show(void *ctx, char *buf, usize cap)
{
	struct zram *z = ctx;

	return zram_show_u64(buf, cap, z->disksize);
}

static isize zram_disksize_store(void *ctx, const char *buf, usize len)
{
	struct zram *z = ctx;
	u64 want = zram_parse_size(buf, len);

	if (!want)
		return -EINVAL;

	int rc = zram_set_disksize(z, want);

	if (rc < 0)
		return rc;
	/* The size changed under a device that already exists: the node's
	 * recorded size is stamped when the nodes are made, so make them again. */
	blk_create_dev_nodes();
	return (isize)len;
}

static isize zram_reset_store(void *ctx, const char *buf, usize len)
{
	struct zram *z = ctx;

	if (!len || buf[0] == '0')
		return -EINVAL;
	/* A device that is in use must not be pulled out from under its user:
	 * the swap layer holds a pointer to it and has pages out on it. */
	/* A device in use must not be pulled out from under its user: the swap
	 * layer holds a pointer to it and has pages out on it. */
	if (swap_is_device(&z->bdev))
		return -EBUSY;
	zram_teardown(z);
	return (isize)len;
}

static isize zram_initstate_show(void *ctx, char *buf, usize cap)
{
	struct zram *z = ctx;

	return zram_show_u64(buf, cap, z->pages ? 1 : 0);
}

static isize zram_algo_show(void *ctx, char *buf, usize cap)
{
	(void)ctx;
	/* The brackets mark the algorithm in use, which is how zramctl reads
	 * the list. There is one, so it is always the selected one. */
	const char *s = "[lz4]\n";
	usize n = strlen(s);

	if (n > cap)
		return -EINVAL;
	memcpy(buf, s, n);
	return (isize)n;
}

/* mm_stat: orig_data_size compr_data_size mem_used_total mem_limit
 * mem_used_max same_pages pages_compacted huge_pages -- the order Linux
 * prints and zramctl parses. mem_limit is 0 (unset) and pages_compacted is 0
 * because nothing here compacts; both are the honest values, not placeholders
 * for something missing. */
static isize zram_mm_stat_show(void *ctx, char *buf, usize cap)
{
	struct zram *z = ctx;
	u64 flags;
	u64 v[8];

	spin_lock_irqsave(&z->lock, &flags);
	v[0] = z->orig_bytes;
	v[1] = z->compr_bytes;
	v[2] = z->compr_bytes + (u64)z->npages * sizeof(struct zram_page);
	v[3] = 0;
	v[4] = z->mem_used_max;
	v[5] = z->same_pages;
	v[6] = 0;
	v[7] = z->huge_pages;
	spin_unlock_irqrestore(&z->lock, flags);

	usize len = 0;

	for (int i = 0; i < 8; i++) {
		isize n = zram_show_u64(buf + len, cap > len ? cap - len : 0, v[i]);

		if (n < 0)
			return n;
		/* zram_show_u64 ends every value with a newline; all but the last
		 * one is a space in this file. */
		buf[len + (usize)n - 1] = (i == 7) ? '\n' : ' ';
		len += (usize)n;
	}
	return (isize)len;
}

static void zram_sysfs_publish(struct zram *z)
{
	struct sysfs_dir *block = sysfs_reg_dir(0, "block");

	if (!block)
		return;
	/* /sys/block is built by the sysfs mount from the block registry; the
	 * registry adopts that directory rather than making a second one, so
	 * these files land beside the ones the block layer publishes. */
	z->sysfs = sysfs_reg_dir(block, z->name);
	if (!z->sysfs)
		return;
	sysfs_reg_attr(z->sysfs, "disksize", 0644, zram_disksize_show,
	               zram_disksize_store, z, 0);
	sysfs_reg_attr(z->sysfs, "reset", 0200, 0, zram_reset_store, z, 0);
	sysfs_reg_attr(z->sysfs, "initstate", 0444, zram_initstate_show, 0, z, 0);
	sysfs_reg_attr(z->sysfs, "comp_algorithm", 0444, zram_algo_show, 0, z, 0);
	sysfs_reg_attr(z->sysfs, "mm_stat", 0444, zram_mm_stat_show, 0, z, 0);
}

void zram_init(void)
{
	for (int i = 0; i < ZRAM_DEVICES; i++) {
		struct zram *z = &g_zram[i];

		snprintf(z->name, sizeof(z->name), "zram%d", i);
		z->lock = SPINLOCK_INIT;
		z->bdev.name = z->name;
		z->bdev.bus = BLK_BUS_MEMORY;
		z->bdev.block_size = 512;
		z->bdev.block_count = 0;
		z->bdev.read_blocks = zram_read_blocks;
		z->bdev.write_blocks = zram_write_blocks;
		z->bdev.priv = z;
		/* Registered now, with no size, exactly as Linux registers zram0
		 * when its module loads: /dev/zram0 and /sys/block/zram0 exist
		 * together or not at all. Publishing only the sysfs half left a
		 * device in /sys/block that nothing could open, which is both a
		 * lie to userspace and a real failure of the rule that every disk
		 * /sys/block names has a node under /dev. */
		blk_register(&z->bdev);
		z->registered = 1;
		zram_sysfs_publish(z);
	}
}
