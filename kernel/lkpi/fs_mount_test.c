/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The proof: mount a real btrfs filesystem through the imported code.
 *
 * This is what the whole shim exists to make possible, and it is a self-test
 * rather than a bridge because the two answer different questions. The bridge
 * will let b1nix's VFS mount a filesystem and serve paths from it; this asks
 * only whether the imported code can read a disk that `mkfs.btrfs` wrote — the
 * superblock, its checksum, the chunk tree and the root tree — and produce a
 * root inode.
 *
 * Everything it exercises is real: the device is opened through b1nix's block
 * layer, every read goes through the block cache, and the checksums are
 * verified by the crc32c in kernel/lkpi/crc32.c. Nothing here is simulated, so
 * a pass means the filesystem was genuinely parsed.
 *
 * Run with `b1nix.lkpi-btrfs-test=<device>`; the smoke harness attaches an
 * image made by mkfs.btrfs on the host and checks it with `btrfs check`
 * afterwards.
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/blkdev.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <lkpi/env.h>

/* The registry in kernel/lkpi/fs_super.c. */
struct file_system_type *get_fs_type(const char *name);

/* lkpi_printk comes from <linux/printk.h>, which the shim already provides;
 * this file is on the Linux side and reaches b1nix's log through it. */
#include <linux/printk.h>


/* ── reading through the mount ───────────────────────────────────── */

/*
 * The mount produced a root dentry; these ask the imported code to do
 * something with it. Each stage is a separate marker because each failure
 * means something different: a lookup that misses is a directory-item problem,
 * a read that returns the wrong bytes is an extent problem, and a readdir that
 * comes back short is a cursor problem.
 *
 * The file is opened by hand rather than through b1nix's VFS because there is
 * no bridge yet — a `struct file` over the inode and its own operations is
 * exactly what the filesystem expects to be handed.
 */
static int lkpi_file_open(struct file *f, struct dentry *dentry)
{
	memset(f, 0, sizeof(*f));
	f->f_inode = dentry->d_inode;
	f->f_mapping = dentry->d_inode->i_mapping;
	f->f_op = dentry->d_inode->i_fop;
	f->f_path.dentry = dentry;
	f->f_mode = FMODE_READ;
	f->f_flags = O_RDONLY;
	atomic_long_set(&f->f_count, 1);
	file_ra_state_init(&f->f_ra, f->f_mapping);
	/*
	 * ->open is not optional here. btrfs_opendir() allocates the buffer
	 * btrfs_real_readdir() builds its entries in and hangs it off
	 * file->private_data; skipping it leaves that NULL, and the readdir
	 * copies a directory entry to whatever the null pointer's fields
	 * happened to contain.
	 */
	if (f->f_op && f->f_op->open)
		return f->f_op->open(f->f_inode, f);
	return 0;
}

static void lkpi_file_close(struct file *f)
{
	if (f->f_op && f->f_op->release)
		f->f_op->release(f->f_inode, f);
}

/* Read at most `len` bytes from `pos` into `buf`; the byte count, or negative. */
static ssize_t lkpi_read_at(struct dentry *dentry, loff_t pos, void *buf,
                            size_t len)
{
	struct file f;
	struct kiocb kiocb;
	struct kvec kvec = { .iov_base = buf, .iov_len = len };
	struct iov_iter iter;

	ssize_t ret = lkpi_file_open(&f, dentry);

	if (ret)
		return ret;
	if (!f.f_op || !f.f_op->read_iter) {
		lkpi_file_close(&f);
		return -EOPNOTSUPP;
	}
	init_sync_kiocb(&kiocb, &f);
	kiocb.ki_pos = pos;
	iov_iter_kvec(&iter, ITER_DEST, &kvec, 1, len);
	ret = f.f_op->read_iter(&kiocb, &iter);
	lkpi_file_close(&f);
	return ret;
}

/* What a directory walk collects: the names, and how many there were. */
struct lkpi_dir_probe {
	struct dir_context ctx;
	int count;
	int saw_hello;
	int saw_big;
	int saw_dir;
	int saw_link;
};

static bool lkpi_dir_actor(struct dir_context *ctx, const char *name, int len,
                           loff_t off, u64 ino, unsigned type)
{
	struct lkpi_dir_probe *p = (struct lkpi_dir_probe *)ctx;

	(void)off;
	(void)ino;
	(void)type;
	p->count++;
	if (len == 10 && memcmp(name, "hello.txt", 9) == 0)
		p->saw_hello = 1;   /* len includes no terminator; 9 is the name */
	if (len == 9 && memcmp(name, "hello.txt", 9) == 0)
		p->saw_hello = 1;
	if (len == 7 && memcmp(name, "big.bin", 7) == 0)
		p->saw_big = 1;
	if (len == 3 && memcmp(name, "dir", 3) == 0)
		p->saw_dir = 1;
	if (len == 8 && memcmp(name, "link.txt", 8) == 0)
		p->saw_link = 1;
	return true;
}

/*
 * Read the filesystem the mount opened: look names up, list the root, read
 * both an inline file and one with real extents, and follow a symlink.
 *
 * The bytes are checked against what the host wrote. big.bin's content is a
 * formula — the sixteen-byte record at offset N holds N/16 — so a read at a
 * high offset proves an extent was resolved rather than a page being served
 * from somewhere convenient.
 */
static int lkpi_btrfs_read_test(struct dentry *root)
{
	struct dentry *hello, *big, *dir, *sub, *deep, *link;
	char buf[64];
	ssize_t n;
	int failures = 0;

	/* The root, listed. */
	{
		struct lkpi_dir_probe probe;
		struct file f;
		int rc;

		memset(&probe, 0, sizeof(probe));
		probe.ctx.actor = lkpi_dir_actor;
		probe.ctx.pos = 0;
		rc = lkpi_file_open(&f, root);
		if (rc) {
			lkpi_printk("LKPI-FS: FAIL btrfs-opendir ret=%d\n", rc);
			failures++;
		} else if (!f.f_op->iterate_shared) {
			lkpi_printk("LKPI-FS: FAIL btrfs-readdir no-iterate\n");
			lkpi_file_close(&f);
			failures++;
		} else {
			rc = f.f_op->iterate_shared(&f, &probe.ctx);
			lkpi_file_close(&f);
			if (rc < 0 || !probe.saw_hello || !probe.saw_big ||
			    !probe.saw_dir || !probe.saw_link) {
				lkpi_printk("LKPI-FS: FAIL btrfs-readdir rc=%d entries=%d "
				            "hello=%d big=%d dir=%d link=%d\n",
				            rc, probe.count, probe.saw_hello, probe.saw_big,
				            probe.saw_dir, probe.saw_link);
				failures++;
			} else {
				lkpi_printk("LKPI-FS: ok btrfs-readdir entries=%d\n",
				            probe.count);
			}
		}
	}

	/* An inline file: small enough that btrfs stores it in its own item. */
	hello = lookup_one_len("hello.txt", root, 9);
	if (IS_ERR(hello) || !hello->d_inode) {
		lkpi_printk("LKPI-FS: FAIL btrfs-lookup hello.txt ret=%d\n",
		            IS_ERR(hello) ? (int)PTR_ERR(hello) : -ENOENT);
		if (!IS_ERR(hello))
			dput(hello);
		failures++;
	} else {
		memset(buf, 0, sizeof(buf));
		n = lkpi_read_at(hello, 0, buf, sizeof(buf));
		if (n != 17 || memcmp(buf, "hello from btrfs\n", 17) != 0) {
			lkpi_printk("LKPI-FS: FAIL btrfs-read-inline n=%d\n", (int)n);
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-read-inline bytes=%d\n", (int)n);
		}
		dput(hello);
	}

	/*
	 * A file with real extents. Each record is fifteen bytes — fourteen
	 * digits and a newline — so the record at offset 15*k holds k, and a
	 * read at a high offset proves an extent was resolved rather than a page
	 * being served from somewhere convenient.
	 */
	big = lookup_one_len("big.bin", root, 7);
	if (IS_ERR(big) || !big->d_inode) {
		lkpi_printk("LKPI-FS: FAIL btrfs-lookup big.bin ret=%d\n",
		            IS_ERR(big) ? (int)PTR_ERR(big) : -ENOENT);
		if (!IS_ERR(big))
			dput(big);
		failures++;
	} else {
		loff_t size = i_size_read(big->d_inode);
		static const struct { loff_t off; const char *want; } probes[] = {
			{ 0,      "00000000000000\n" },
			{ 15 * 1000, "00000000001000\n" },
			{ 15 * 8000, "00000000008000\n" },
		};

		if (size != 184320) {
			lkpi_printk("LKPI-FS: FAIL btrfs-read-extent size=%ld\n",
			            (long)size);
			failures++;
		}
		for (unsigned p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
			memset(buf, 0, sizeof(buf));
			n = lkpi_read_at(big, probes[p].off, buf, 15);
			if (n != 15 || memcmp(buf, probes[p].want, 15) != 0) {
				lkpi_printk("LKPI-FS: FAIL btrfs-read-extent off=%ld n=%d\n",
				            (long)probes[p].off, (int)n);
				failures++;
			} else {
				lkpi_printk("LKPI-FS: ok btrfs-read-extent off=%ld\n",
				            (long)probes[p].off);
			}
		}
		dput(big);
	}

	/* A directory two levels down, and the file in it. */
	dir = lookup_one_len("dir", root, 3);
	sub = (!IS_ERR(dir) && dir->d_inode) ? lookup_one_len("sub", dir, 3)
	                                     : ERR_PTR(-ENOENT);
	deep = (!IS_ERR(sub) && sub->d_inode) ? lookup_one_len("deep.txt", sub, 8)
	                                      : ERR_PTR(-ENOENT);
	if (IS_ERR(deep) || !deep->d_inode) {
		lkpi_printk("LKPI-FS: FAIL btrfs-nested ret=%d\n",
		            IS_ERR(deep) ? (int)PTR_ERR(deep) : -ENOENT);
		failures++;
	} else {
		memset(buf, 0, sizeof(buf));
		n = lkpi_read_at(deep, 0, buf, sizeof(buf));
		if (n != 7 || memcmp(buf, "nested\n", 7) != 0) {
			lkpi_printk("LKPI-FS: FAIL btrfs-nested-read n=%d\n", (int)n);
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-nested\n");
		}
	}
	if (!IS_ERR(deep))
		dput(deep);
	if (!IS_ERR(sub))
		dput(sub);
	if (!IS_ERR(dir))
		dput(dir);

	/* A symlink, whose target lives in the inode rather than in a block. */
	link = lookup_one_len("link.txt", root, 8);
	if (IS_ERR(link) || !link->d_inode) {
		lkpi_printk("LKPI-FS: FAIL btrfs-lookup link.txt ret=%d\n",
		            IS_ERR(link) ? (int)PTR_ERR(link) : -ENOENT);
		if (!IS_ERR(link))
			dput(link);
		failures++;
	} else {
		struct inode *li = link->d_inode;
		const char *target = NULL;
		struct delayed_call done = { 0, 0 };

		if (!S_ISLNK(li->i_mode)) {
			lkpi_printk("LKPI-FS: FAIL btrfs-symlink mode=%o\n", li->i_mode);
			failures++;
		} else {
			if (li->i_op && li->i_op->get_link)
				target = li->i_op->get_link(link, li, &done);
			if (!target || IS_ERR(target) ||
			    strcmp(target, "hello.txt") != 0) {
				lkpi_printk("LKPI-FS: FAIL btrfs-symlink target=%s\n",
				            (target && !IS_ERR(target)) ? target : "(none)");
				failures++;
			} else {
				lkpi_printk("LKPI-FS: ok btrfs-symlink\n");
			}
			do_delayed_call(&done);
		}
		dput(link);
	}

	return failures;
}


/* ── writing through the mount ───────────────────────────────────── */

/*
 * Create a file, write to it, sync it, read it back.
 *
 * This is the other half of the proof, and the harder one: a write has to
 * reserve space, allocate an extent, checksum the data, dirty the pages and
 * then get all of it to the disk through a transaction commit. The host's
 * `btrfs check` after the run is the judge — if any of that is wrong, it says
 * so about the filesystem rather than about us.
 */
static int lkpi_btrfs_write_test(struct dentry *root)
{
	static const char payload[] =
		"written by b1nix through the imported btrfs\n";
	const int payload_len = (int)sizeof(payload) - 1;
	struct dentry *dentry;
	struct inode *dir = root->d_inode;
	struct file f;
	struct kiocb kiocb;
	struct kvec kvec;
	struct iov_iter iter;
	char buf[64];
	ssize_t n;
	int rc;
	int failures = 0;

	if (!dir->i_op || !dir->i_op->create) {
		lkpi_printk("LKPI-FS: FAIL btrfs-create no-op\n");
		return 1;
	}

	dentry = lookup_one_len("written.txt", root, 11);
	if (IS_ERR(dentry)) {
		lkpi_printk("LKPI-FS: FAIL btrfs-create lookup=%d\n",
		            (int)PTR_ERR(dentry));
		return 1;
	}
	if (dentry->d_inode) {
		/* A leftover from an earlier run: the image is made fresh for each,
		 * so this means the create below is not testing what it says. */
		lkpi_printk("LKPI-FS: FAIL btrfs-create already-exists\n");
		dput(dentry);
		return 1;
	}

	rc = dir->i_op->create(&nop_mnt_idmap, dir, dentry, S_IFREG | 0644, 0);
	if (rc || !dentry->d_inode) {
		lkpi_printk("LKPI-FS: FAIL btrfs-create ret=%d\n", rc);
		dput(dentry);
		return 1;
	}
	lkpi_printk("LKPI-FS: ok btrfs-create ino=%lu\n",
	            (unsigned long)dentry->d_inode->i_ino);

	/* Write. */
	rc = lkpi_file_open(&f, dentry);
	if (rc) {
		lkpi_printk("LKPI-FS: FAIL btrfs-write open=%d\n", rc);
		dput(dentry);
		return 1;
	}
	f.f_mode = FMODE_READ | FMODE_WRITE;
	f.f_flags = O_RDWR;
	if (!f.f_op || !f.f_op->write_iter) {
		lkpi_printk("LKPI-FS: FAIL btrfs-write no-write_iter\n");
		lkpi_file_close(&f);
		dput(dentry);
		return 1;
	}
	kvec.iov_base = (void *)payload;
	kvec.iov_len = (size_t)payload_len;
	init_sync_kiocb(&kiocb, &f);
	kiocb.ki_pos = 0;
	iov_iter_kvec(&iter, ITER_SOURCE, &kvec, 1, (size_t)payload_len);
	n = f.f_op->write_iter(&kiocb, &iter);
	if (n != payload_len) {
		lkpi_printk("LKPI-FS: FAIL btrfs-write n=%d\n", (int)n);
		failures++;
	} else {
		lkpi_printk("LKPI-FS: ok btrfs-write bytes=%d\n", (int)n);
	}

	/* Sync, which is where the extent and its checksum reach the disk. */
	if (failures == 0) {
		if (!f.f_op->fsync) {
			lkpi_printk("LKPI-FS: FAIL btrfs-fsync no-op\n");
			failures++;
		} else {
			rc = f.f_op->fsync(&f, 0, LLONG_MAX, 0);
			if (rc) {
				lkpi_printk("LKPI-FS: FAIL btrfs-fsync ret=%d\n", rc);
				failures++;
			} else {
				lkpi_printk("LKPI-FS: ok btrfs-fsync\n");
			}
		}
	}
	lkpi_file_close(&f);

	/* Read it back — from the page cache, but through the filesystem. */
	if (failures == 0) {
		memset(buf, 0, sizeof(buf));
		n = lkpi_read_at(dentry, 0, buf, sizeof(buf));
		if (n != payload_len || memcmp(buf, payload, (size_t)payload_len) != 0) {
			lkpi_printk("LKPI-FS: FAIL btrfs-write-readback n=%d\n", (int)n);
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-write-readback size=%ld\n",
			            (long)i_size_read(dentry->d_inode));
		}
	}

	/* A directory, so the host check sees more than one kind of change. */
	if (failures == 0 && dir->i_op->mkdir) {
		struct dentry *d = lookup_one_len("made", root, 4);

		if (IS_ERR(d) || d->d_inode) {
			lkpi_printk("LKPI-FS: FAIL btrfs-mkdir lookup\n");
			failures++;
			if (!IS_ERR(d))
				dput(d);
		} else {
			struct dentry *de = dir->i_op->mkdir(&nop_mnt_idmap, dir, d,
			                                     S_IFDIR | 0755);

			rc = IS_ERR(de) ? (int)PTR_ERR(de) : 0;
			if (!IS_ERR_OR_NULL(de)) {
				dput(d);
				d = de;
			}
			if (rc || !d->d_inode) {
				lkpi_printk("LKPI-FS: FAIL btrfs-mkdir ret=%d\n", rc);
				failures++;
			} else {
				lkpi_printk("LKPI-FS: ok btrfs-mkdir\n");
			}
			dput(d);
		}
	}

	dput(dentry);
	return failures;
}


/* Write `len` bytes at `pos`; the byte count, or negative. */
static ssize_t lkpi_write_at(struct dentry *dentry, loff_t pos,
                             const void *buf, size_t len)
{
	struct file f;
	struct kiocb kiocb;
	struct kvec kvec = { .iov_base = (void *)buf, .iov_len = len };
	struct iov_iter iter;
	ssize_t ret = lkpi_file_open(&f, dentry);

	if (ret)
		return ret;
	f.f_mode = FMODE_READ | FMODE_WRITE;
	f.f_flags = O_RDWR;
	if (!f.f_op || !f.f_op->write_iter) {
		lkpi_file_close(&f);
		return -EOPNOTSUPP;
	}
	init_sync_kiocb(&kiocb, &f);
	kiocb.ki_pos = pos;
	iov_iter_kvec(&iter, ITER_SOURCE, &kvec, 1, len);
	ret = f.f_op->write_iter(&kiocb, &iter);
	if (ret > 0 && f.f_op->fsync) {
		int rc = f.f_op->fsync(&f, 0, LLONG_MAX, 0);

		if (rc)
			ret = rc;
	}
	lkpi_file_close(&f);
	return ret;
}

/*
 * The rest of the namespace and the rest of the write path.
 *
 * A single small file proves an extent was allocated; these prove the
 * operations a filesystem is actually used through — a write big enough to
 * need several pages and its own checksums, a file grown by appending, one
 * truncated, renamed, hard-linked, symlinked and finally removed.
 */
static int lkpi_btrfs_write_more(struct dentry *root)
{
	struct inode *dir = root->d_inode;
	struct dentry *big, *renamed = NULL, *linked, *slink, *gone;
	static char pattern[3 * 4096];
	char check[64];
	ssize_t n;
	int rc;
	int failures = 0;
	unsigned i;

	/* Content derivable from the offset, so a read-back that is merely
	 * plausible does not pass. */
	for (i = 0; i < sizeof(pattern); i++)
		pattern[i] = (char)('A' + (i % 26));

	big = lookup_one_len("multi.bin", root, 9);
	if (IS_ERR(big) || big->d_inode) {
		lkpi_printk("LKPI-FS: FAIL btrfs-multi lookup\n");
		if (!IS_ERR(big))
			dput(big);
		return 1;
	}
	rc = dir->i_op->create(&nop_mnt_idmap, dir, big, S_IFREG | 0644, 0);
	if (rc) {
		lkpi_printk("LKPI-FS: FAIL btrfs-multi create=%d\n", rc);
		dput(big);
		return 1;
	}
	n = lkpi_write_at(big, 0, pattern, sizeof(pattern));
	if (n != (ssize_t)sizeof(pattern)) {
		lkpi_printk("LKPI-FS: FAIL btrfs-multi write n=%d\n", (int)n);
		failures++;
	} else {
		memset(check, 0, sizeof(check));
		n = lkpi_read_at(big, 8192 + 100, check, 26);
		if (n != 26 || memcmp(check, pattern + 8192 + 100, 26) != 0) {
			lkpi_printk("LKPI-FS: FAIL btrfs-multi readback n=%d\n", (int)n);
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-multi-page bytes=%d\n",
			            (int)sizeof(pattern));
		}
	}

	/* Overwrite a page in the middle: the same range, written again. */
	n = lkpi_write_at(big, 4096, "MIDDLE", 6);
	if (n != 6) {
		lkpi_printk("LKPI-FS: FAIL btrfs-rewrite n=%d\n", (int)n);
		failures++;
	} else {
		memset(check, 0, sizeof(check));
		n = lkpi_read_at(big, 4096, check, 6);
		if (n != 6 || memcmp(check, "MIDDLE", 6) != 0) {
			lkpi_printk("LKPI-FS: FAIL btrfs-rewrite readback\n");
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-rewrite\n");
		}
	}

	/* Append past the end, which grows the file and allocates again. */
	n = lkpi_write_at(big, (loff_t)sizeof(pattern), "tail", 4);
	if (n != 4 || i_size_read(big->d_inode) != (loff_t)sizeof(pattern) + 4) {
		lkpi_printk("LKPI-FS: FAIL btrfs-append n=%d size=%ld\n", (int)n,
		            (long)i_size_read(big->d_inode));
		failures++;
	} else {
		lkpi_printk("LKPI-FS: ok btrfs-append size=%ld\n",
		            (long)i_size_read(big->d_inode));
	}

	/* Truncate, which frees extents rather than allocating them. */
	{
		struct iattr attr;

		memset(&attr, 0, sizeof(attr));
		attr.ia_valid = ATTR_SIZE;
		attr.ia_size = 4096;
		inode_lock(big->d_inode);
		rc = big->d_inode->i_op->setattr
		         ? big->d_inode->i_op->setattr(&nop_mnt_idmap, big, &attr)
		         : -EOPNOTSUPP;
		inode_unlock(big->d_inode);
		/* And again to a size that is NOT a block boundary, which is the
		 * path that has to zero the tail of the last block rather than
		 * only drop whole extents. */
		if (rc == 0) {
			memset(&attr, 0, sizeof(attr));
			attr.ia_valid = ATTR_SIZE;
			attr.ia_size = 4;
			inode_lock(big->d_inode);
			rc = big->d_inode->i_op->setattr(&nop_mnt_idmap, big, &attr);
			inode_unlock(big->d_inode);
			if (rc == 0 && i_size_read(big->d_inode) != 4)
				rc = -1;
		}
		if (rc || i_size_read(big->d_inode) != 4) {
			lkpi_printk("LKPI-FS: FAIL btrfs-truncate ret=%d size=%ld\n", rc,
			            (long)i_size_read(big->d_inode));
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-truncate\n");
		}
	}

	/* Rename it. */
	renamed = lookup_one_len("multi-renamed.bin", root, 17);
	if (IS_ERR(renamed) || renamed->d_inode || !dir->i_op->rename) {
		lkpi_printk("LKPI-FS: FAIL btrfs-rename lookup\n");
		if (!IS_ERR(renamed))
			dput(renamed);
		renamed = NULL;
		failures++;
	} else {
		rc = dir->i_op->rename(&nop_mnt_idmap, dir, big, dir, renamed, 0);
		if (rc) {
			lkpi_printk("LKPI-FS: FAIL btrfs-rename ret=%d\n", rc);
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-rename\n");
		}
	}

	/* A hard link to it. */
	linked = lookup_one_len("multi-link.bin", root, 14);
	if (!IS_ERR(linked) && !linked->d_inode && dir->i_op->link) {
		rc = dir->i_op->link(renamed && renamed->d_inode ? renamed : big, dir,
		                     linked);
		if (rc || !linked->d_inode) {
			lkpi_printk("LKPI-FS: FAIL btrfs-link ret=%d\n", rc);
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-link nlink=%u\n",
			            (unsigned)linked->d_inode->i_nlink);
		}
	} else {
		lkpi_printk("LKPI-FS: FAIL btrfs-link lookup\n");
		failures++;
	}

	/* A symlink beside it. */
	slink = lookup_one_len("made-link", root, 9);
	if (!IS_ERR(slink) && !slink->d_inode && dir->i_op->symlink) {
		rc = dir->i_op->symlink(&nop_mnt_idmap, dir, slink, "written.txt");
		if (rc || !slink->d_inode) {
			lkpi_printk("LKPI-FS: FAIL btrfs-symlink-create ret=%d\n", rc);
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-symlink-create\n");
		}
	} else {
		lkpi_printk("LKPI-FS: FAIL btrfs-symlink-create lookup\n");
		failures++;
	}

	/* And remove one, so the host's check sees a freed reference too. */
	gone = lookup_one_len("multi-link.bin", root, 14);
	if (!IS_ERR(gone) && gone->d_inode && dir->i_op->unlink) {
		rc = dir->i_op->unlink(dir, gone);
		if (rc) {
			lkpi_printk("LKPI-FS: FAIL btrfs-unlink ret=%d\n", rc);
			failures++;
		} else {
			lkpi_printk("LKPI-FS: ok btrfs-unlink\n");
		}
	} else {
		lkpi_printk("LKPI-FS: FAIL btrfs-unlink lookup\n");
		failures++;
	}
	if (!IS_ERR(gone))
		dput(gone);
	if (!IS_ERR(slink))
		dput(slink);
	if (!IS_ERR(linked))
		dput(linked);
	if (renamed && !IS_ERR(renamed))
		dput(renamed);
	dput(big);
	return failures;
}

/*
 * Mount, report, unmount.
 *
 * The report is a marker line the harness greps for, in the shape the rest of
 * the suite uses. Each stage is reported separately because each failure means
 * something different: no type means the filesystem never registered, a failed
 * get_tree means the disk could not be read, and a NULL root means it was read
 * and produced nothing.
 */
int lkpi_btrfs_mount_test(const char *device)
{
	struct file_system_type *type;
	struct dentry *root;
	struct super_block *sb;

	type = get_fs_type("btrfs");
	if (!type) {
		lkpi_printk("LKPI-FS: FAIL btrfs-registered\n");
		return -ENOENT;
	}
	lkpi_printk("LKPI-FS: ok btrfs-registered\n");

	/*
	 * btrfs uses the OLD mount interface in 6.6 — `->mount`, not
	 * `->init_fs_context`. Both are real entry points and a filesystem has
	 * exactly one; taking the wrong branch is how a mount fails with EINVAL
	 * before the disk is touched.
	 */
	if (!type->mount) {
		lkpi_printk("LKPI-FS: FAIL btrfs-mount-op\n");
		return -EINVAL;
	}
	lkpi_printk("LKPI-FS: ok btrfs-mount-op\n");

	/*
	 * Open the device here first, and report it separately.
	 *
	 * A mount failure otherwise says only that something went wrong, and the
	 * two candidates — the device could not be found, or the filesystem on it
	 * could not be read — need different fixes. This distinguishes them
	 * before btrfs is entered.
	 */
	{
		struct block_device *probe =
			blkdev_get_by_path(device, BLK_OPEN_READ, &probe, NULL);

		if (IS_ERR(probe)) {
			lkpi_printk("LKPI-FS: FAIL btrfs-device ret=%d dev=%s\n",
			            (int)PTR_ERR(probe), device);
			return (int)PTR_ERR(probe);
		}
		lkpi_printk("LKPI-FS: ok btrfs-device sectors=%lu bs=%u\n",
		            (unsigned long)bdev_nr_sectors(probe),
		            bdev_logical_block_size(probe));

		/*
		 * The superblock, read through the device's own page cache — the
		 * same call btrfs makes, at the same offset, twice with the cache
		 * dropped in between.
		 *
		 * It is checked here because the mount is the first thing that
		 * exercises it and the mount reports only that it failed. A cache
		 * that returns a page it never filled, or fills it from the wrong
		 * sector, produces a superblock whose magic or self-recorded
		 * address is wrong, and btrfs answers that with a bare -EINVAL.
		 */
		for (int pass = 0; pass < 2; pass++) {
			struct page *page;
			const unsigned char *sb_bytes;
			unsigned long long magic = 0, self = 0;

			if (pass == 1)
				invalidate_bdev(probe);
			page = read_cache_page_gfp(probe->bd_inode->i_mapping,
			                           (pgoff_t)(65536 >> PAGE_SHIFT),
			                           GFP_KERNEL);
			if (IS_ERR(page)) {
				lkpi_printk("LKPI-FS: FAIL btrfs-super-read pass=%d ret=%d\n",
				            pass, (int)PTR_ERR(page));
				blkdev_put(probe, &probe);
				return (int)PTR_ERR(page);
			}
			sb_bytes = (const unsigned char *)page_address(page);
			/* magic at offset 0x40, the superblock's own address at 0x30 —
			 * both little-endian on disk. */
			for (int i = 7; i >= 0; i--)
				magic = (magic << 8) | sb_bytes[0x40 + i];
			for (int i = 7; i >= 0; i--)
				self = (self << 8) | sb_bytes[0x30 + i];
			lkpi_printk("LKPI-FS: %s btrfs-super pass=%d magic=%llx bytenr=%llu\n",
			            (magic == 0x4d5f53665248425fULL && self == 65536)
			                ? "ok" : "FAIL",
			            pass, magic, self);
			put_page(page);
			if (magic != 0x4d5f53665248425fULL || self != 65536) {
				blkdev_put(probe, &probe);
				return -EINVAL;
			}
		}
		blkdev_put(probe, &probe);
	}

	/*
	 * Read-only, and with no options: the image the harness made has none,
	 * and a read-only mount proves the read path without changing the bytes
	 * the host is about to check.
	 */
	root = type->mount(type, SB_RDONLY, device, NULL);
	if (IS_ERR(root)) {
		lkpi_printk("LKPI-FS: FAIL btrfs-mount ret=%d\n", (int)PTR_ERR(root));
		return (int)PTR_ERR(root);
	}

	if (!root || !root->d_inode) {
		lkpi_printk("LKPI-FS: FAIL btrfs-root\n");
		return -EIO;
	}
	sb = root->d_sb;

	/*
	 * What the mount actually produced. The values are printed rather than
	 * merely asserted non-zero: a root inode number that is not 256 means
	 * btrfs found something other than its default subvolume's root, and the
	 * block size is what tells us the superblock was parsed rather than
	 * guessed.
	 */
	lkpi_printk("LKPI-FS: ok btrfs-mount root_ino=%lu blocksize=%lu magic=%lx\n",
	            (unsigned long)root->d_inode->i_ino,
	            (unsigned long)sb->s_blocksize,
	            (unsigned long)sb->s_magic);

	if (!S_ISDIR(root->d_inode->i_mode)) {
		lkpi_printk("LKPI-FS: FAIL btrfs-root-isdir mode=%o\n",
		            root->d_inode->i_mode);
		return -ENOTDIR;
	}
	lkpi_printk("LKPI-FS: ok btrfs-root-isdir\n");

	if (lkpi_btrfs_read_test(root) != 0) {
		/* Reported per stage above; the mount is still taken down, because a
		 * failed read is not a reason to leave a filesystem mounted. */
		dput(root);
		deactivate_super(sb);
		return -EIO;
	}

	/*
	 * Unmount. It is part of the test rather than cleanup: put_super is where
	 * btrfs writes anything it owes and frees its trees, and a mount that
	 * cannot be undone is not a working mount.
	 */
	dput(root);
	deactivate_super(sb);
	lkpi_printk("LKPI-FS: ok btrfs-umount\n");

	/*
	 * Then again, writable. Mounting a second time is deliberate: it proves
	 * the first mount really let go of the device, and everything the write
	 * path needs — space reservation, extent allocation — is refused on the
	 * read-only mount above, where every block group is read-only.
	 */
	root = type->mount(type, 0, device, NULL);
	if (IS_ERR(root)) {
		lkpi_printk("LKPI-FS: FAIL btrfs-mount-rw ret=%d\n",
		            (int)PTR_ERR(root));
		return (int)PTR_ERR(root);
	}
	if (!root || !root->d_inode) {
		lkpi_printk("LKPI-FS: FAIL btrfs-mount-rw no-root\n");
		return -EIO;
	}
	sb = root->d_sb;
	lkpi_printk("LKPI-FS: ok btrfs-mount-rw root_ino=%lu\n",
	            (unsigned long)root->d_inode->i_ino);

	{
		int failed = lkpi_btrfs_write_test(root);

		if (!failed)
			failed = lkpi_btrfs_write_more(root);

		dput(root);
		deactivate_super(sb);
		if (failed)
			return -EIO;
	}
	lkpi_printk("LKPI-FS: ok btrfs-umount-rw\n");
	return 0;
}

/*
 * The thread entry.
 *
 * The test runs on a kernel thread because a mount waits for the filesystem's
 * own workers, and nothing can wait before the scheduler is running — see the
 * note at the call site in kernel/main.c.
 */
/* Raised when the test has finished, so the watchdog beside it stays quiet. */
static volatile int lkpi_btrfs_test_done;

void lkpi_btrfs_mount_test_thread(void *arg)
{
	lkpi_btrfs_mount_test((const char *)arg);
	lkpi_btrfs_test_done = 1;
	lkpi_printk("LKPI-FS: done\n");
	for (;;)
		lkpi_yield();
}

/*
 * The watchdog beside the mount.
 *
 * A wedged mount reports nothing on its own: the thread stays runnable, so it
 * shows up in a task dump as merely running and no wait names the place. This
 * waits, then asks the kernel profiler where the CPU actually is — which turns
 * "the mount hangs" into a function name.
 */
void lkpi_kprof_dump(void);
void lkpi_sleep_ticks(u64 ticks);
void lkpi_task_dump(void);
void lkpi_wq_dump(void);

void lkpi_btrfs_mount_watch_thread(void *arg)
{
	(void)arg;
	/* 20 seconds at the 1 kHz tick. Long enough that a slow mount is not
	 * mistaken for a wedged one. */
	lkpi_sleep_ticks(20000);
	if (lkpi_btrfs_test_done) {
		/* The test finished; there is nothing to report and a report would
		 * only be noise in the log the harness greps. */
		for (;;)
			lkpi_sleep_ticks(10000);
	}
	lkpi_printk("LKPI-FS: mount watchdog — profile follows\n");
	/* The cheap reports first: the profile's raw sample list is long enough
	 * that a run can end before the rest of it reaches the log. */
	lkpi_wq_dump();
	lkpi_task_dump();
	lkpi_kprof_dump();
	for (;;)
		lkpi_sleep_ticks(10000);
}
