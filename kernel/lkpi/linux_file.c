/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: the file and descriptor bridge.
 *
 * Imported code hands userspace a buffer or a fence by wrapping it in a file
 * with no name in any directory and installing a descriptor for it. b1nix has
 * all the pieces — vfs_handle, the per-process fd table, handle refcounting —
 * so this is a translation rather than an implementation: a Linux `struct file`
 * is allocated here and carried in the handle's private_data, and the handle is
 * what the fd table stores.
 *
 * The two-phase pattern Linux uses (reserve a descriptor, build the file,
 * install it) is preserved rather than collapsed. It exists so a failure
 * between the two steps releases the descriptor instead of leaking it, and
 * collapsing the pair would quietly remove that property from every caller.
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <lkpi/env.h>

/*
 * No b1nix headers here, on purpose. This file defines `current` and
 * `task_tgid` through <linux/types.h>, and <b1nix/sched.h> has its own — a
 * translation unit that sees both fails on identifiers neither side owns. The
 * descriptor calls in <lkpi/env.h> are the crossing point; see the note there.
 */

/* ── inodes ─────────────────────────────────────────────────────── */

static volatile u64 g_anon_ino;

struct inode *alloc_anon_inode(struct super_block *sb)
{
	(void)sb;
	struct inode *inode = (struct inode *)lkpi_kmalloc(sizeof(*inode),
	                                                   GFP_KERNEL | __GFP_ZERO);
	if (!inode)
		return 0;
	/* A number nothing else will hand out. Anonymous inodes are compared for
	 * identity — drm_prime uses that to recognise its own buffers — so the
	 * value only has to be unique, not meaningful. */
	inode->i_ino = (unsigned long)__atomic_add_fetch(&g_anon_ino, 1ull,
	                                                 __ATOMIC_ACQ_REL);
	return inode;
}

void iput(struct inode *inode)
{
	lkpi_kfree(inode);
}

/* ── files ──────────────────────────────────────────────────────── */

/* The handle the fd table stores. Its private_data is the Linux file, which is
 * how a descriptor gets back to the object imported code cares about. */
struct file *anon_inode_getfile(const char *name,
                                const struct file_operations *fops, void *priv,
                                int flags)
{
	(void)name;

	struct file *f = (struct file *)lkpi_kmalloc(sizeof(*f),
	                                             GFP_KERNEL | __GFP_ZERO);
	if (!f)
		return 0;

	f->f_inode = alloc_anon_inode(0);
	if (!f->f_inode) {
		lkpi_kfree(f);
		return 0;
	}
	f->f_op = fops;
	f->private_data = priv;
	f->f_flags = (unsigned int)flags;
	f->f_mode = FMODE_READ | FMODE_WRITE;
	atomic64_set(&f->f_count, 1);
	return f;
}

void fput(struct file *f)
{
	if (!f)
		return;
	if (atomic64_sub_return(1, &f->f_count) != 0)
		return;

	/* Last reference: let the owner tear its object down before the file that
	 * named it goes away. */
	if (f->f_op && f->f_op->release)
		f->f_op->release(f->f_inode, f);
	iput(f->f_inode);
	lkpi_kfree(f);
}

struct file *fget(unsigned int fd)
{
	void *h = lkpi_fd_lookup((int)fd);
	if (!h)
		return 0;
	struct file *f = (struct file *)lkpi_handle_private(h);
	if (!f)
		return 0;
	atomic64_inc(&f->f_count);
	return f;
}

struct fd fdget(unsigned int fd)
{
	struct fd out = { 0, 0 };
	out.file = fget(fd);
	/* The flag records that fdput owes a put. fget always took one, so it
	 * always does — b1nix has no "borrowed without a reference" fast path. */
	out.flags = out.file ? 1u : 0u;
	return out;
}

void fdput(struct fd f)
{
	if (f.flags && f.file)
		fput(f.file);
}

struct file *file_clone_open(struct file *f)
{
	if (!f)
		return 0;
	/* A second reference to the same object, not a copy: the two descriptors
	 * must share the driver's private_data, which is the point of cloning. */
	atomic64_inc(&f->f_count);
	return f;
}

/* ── descriptors ────────────────────────────────────────────────── */

/*
 * Reserving a descriptor needs a handle to put in the table, so a placeholder
 * one is allocated here and filled in by fd_install. That keeps the reserve /
 * install pair meaningful: a caller that fails in between calls put_unused_fd
 * and the descriptor really is released.
 */
int get_unused_fd_flags(unsigned int flags)
{
		(void)flags;
	void *h = lkpi_handle_alloc();
	if (!h)
		return -EMFILE;

	int fd = lkpi_fd_install(h);
	if (fd < 0) {
		lkpi_handle_release(h);
		return -EMFILE;
	}
	return fd;
}

void fd_install(unsigned int fd, struct file *f)
{
	void *h = lkpi_fd_lookup((int)fd);
	if (!h || !f)
		return;
	lkpi_handle_set_private(h, f);
	f->f_handle = h;
}

void put_unused_fd(unsigned int fd)
{
	lkpi_fd_close((int)fd);
}

/* ── mappings ───────────────────────────────────────────────────── */

void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin,
                         loff_t const holelen, int even_cows)
{
	(void)mapping;
	(void)holebegin;
	(void)holelen;
	(void)even_cows;
	/*
	 * Tearing down userspace mappings of a range so the next access faults back
	 * into the driver. b1nix's VMA teardown is per-process and the DRM mmap
	 * path is not wired to it yet, so there is nothing mapped to tear down —
	 * and there will not be until that path exists. Doing nothing is correct
	 * for the current configuration and wrong the moment mmap lands, which is
	 * why the two have to land together.
	 */
}

/* ── superblock ─────────────────────────────────────────────────── */

void kill_anon_super(struct super_block *sb)
{
	(void)sb;
	/* Nothing allocated one: init_pseudo returns NULL and simple_pin_fs never
	 * mounts anything, so there is no superblock to destroy. */
}

int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount,
                  int *count)
{
	(void)type;
	if (count)
		(*count)++;
	if (mount)
		*mount = 0;
	/*
	 * Anonymous inodes here are allocated directly rather than from a mounted
	 * pseudo-filesystem, so there is nothing to pin. Success is reported
	 * because the caller's only use of the mount is to pass it to
	 * alloc_anon_inode, which ignores it.
	 */
	return 0;
}

void simple_release_fs(struct vfsmount **mount, int *count)
{
	if (count && *count > 0)
		(*count)--;
	if (mount)
		*mount = 0;
}

/* ── shmem ──────────────────────────────────────────────────────── */

struct file *shmem_file_setup(const char *name, loff_t size,
                              unsigned long flags)
{
	(void)name;
	(void)size;
	(void)flags;
	/*
	 * A file backing anonymous, swappable memory. b1nix's equivalent is
	 * lkpi's shmem_alloc_pages, which hands back the page array directly —
	 * so a driver that takes this path gets nothing, deliberately, rather
	 * than a file whose pages nobody allocates. GEM objects here are backed
	 * through the page array instead.
	 */
	return ERR_PTR(-ENOSYS);
}

struct folio *shmem_read_folio_gfp(struct address_space *mapping,
                                   unsigned long index, gfp_t gfp)
{
	(void)mapping;
	(void)index;
	(void)gfp;
	/* Same reason: there is no file-backed mapping to fault a page out of. */
	return 0;
}
