/*
 * SPDX-License-Identifier: GPL-2.0-only
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
#include <linux/printk.h>
#include <lkpi/env.h>
#include <lkpi/drmdev.h>
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
	/* NULL is not an error: a file whose release owns the inode's storage
	 * clears the pointer, precisely so this does not free into the middle of
	 * someone else's allocation. */
	if (inode)
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
	/* A genuinely separate file, with its own driver state. The lease path
	 * rewrites the clone's master, so sharing one object would have rewritten
	 * the original's — see lkpi_drm_clone_file. */
	struct file *clone = (struct file *)lkpi_drm_clone_file(f);

	if (!clone)
		return (struct file *)ERR_PTR(-EOPNOTSUPP);
	return clone;
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
	/* A cloned file already knows the descriptor it was first opened through;
	 * the new one takes that file's device node so the two are the same device
	 * as far as anything that stats them is concerned. */
	if (f->f_handle && f->f_handle != h) {
		lkpi_handle_inherit_node(h, f->f_handle);
	} else {
		/* A file the core opened for itself carries no handle to copy from, so
		 * the identity comes from the device it belongs to. Without it the
		 * descriptor stats as an anonymous file and wlroots rejects the card as
		 * "not a primary DRM node". */
		u32 minor = 0;

		if (lkpi_drm_file_minor(f, &minor))
			lkpi_handle_attach_drm_minor(h, minor);
	}
	lkpi_handle_set_private(h, f);
	if (!f->f_handle)
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

/*
 * A shmem file: anonymous pages addressed by index, with a struct file in front
 * of them.
 *
 * This is what backs a GEM object. i915 does not want the pages directly — it
 * wants obj->base.filp, and reaches the pages through filp->f_mapping one index
 * at a time, so the file and its address_space have to be real objects even
 * though nothing here is a filesystem.
 *
 * Pages are allocated on first touch rather than up front: an object is often
 * created much larger than the part that is ever populated, and i915 asks for
 * each index as it needs it. They are freed when the file's last reference goes
 * or when the range is truncated.
 *
 * Not swappable, unlike the name. There is nothing to swap to — see
 * shmem_read_mapping_page_gfp() in linux_support.c — so these pages stay
 * resident for the life of the object.
 */
struct lkpi_shmem {
	struct file file;
	struct inode inode;
	struct address_space mapping;
	usize npages;
	/*
	 * One contiguous array of struct page, frames allocated per slot as it is
	 * touched. Contiguous is the requirement, not a convenience: i915
	 * coalesces physically adjacent pages into a single scatterlist entry and
	 * then walks that entry with nth_page(), which is pointer arithmetic on
	 * struct page. An array of pointers to individually allocated pages made
	 * that walk dereference a wild pointer on the second page of every
	 * coalesced run.
	 */
	struct page *pages;
};

struct folio *shmem_read_folio_gfp(struct address_space *mapping,
                                   unsigned long index, gfp_t gfp);

static struct lkpi_shmem *shmem_from_mapping(struct address_space *mapping)
{
	return mapping ? container_of(mapping, struct lkpi_shmem, mapping) : 0;
}

static void shmem_drop_range(struct lkpi_shmem *sh, usize first, usize last)
{
	if (!sh || !sh->pages)
		return;
	if (last >= sh->npages)
		last = sh->npages ? sh->npages - 1 : 0;
	for (usize i = first; i < sh->npages && i <= last; i++)
		lkpi_pagevec_release(sh->pages, i);
}

static int shmem_file_release(struct inode *inode, struct file *file)
{
	struct lkpi_shmem *sh;

	(void)inode;
	if (!file)
		return 0;
	sh = container_of(file, struct lkpi_shmem, file);
	lkpi_pagevec_free(sh->pages, sh->npages);
	sh->pages = 0;
	/*
	 * The inode is embedded in this object, not separately allocated, so it
	 * must not be freed on its own — clearing the pointer is what stops
	 * fput()'s iput() from freeing into the middle of this allocation. The
	 * object itself is freed by fput()'s kfree, which is correct because
	 * struct file is its first member.
	 */
	file->f_inode = 0;
	return 0;
}

/* Hand back the page for this offset so the caller can copy into it. The pair
 * exists because the page cache normally has to be told a write is coming;
 * here the page is simply allocated if it is not there yet. */
static int shmem_write_begin(struct file *file, struct address_space *mapping,
                             loff_t pos, unsigned len, struct page **pagep,
                             void **fsdata)
{
	struct folio *folio;

	(void)file;
	(void)len;
	(void)fsdata;
	folio = shmem_read_folio_gfp(mapping, (unsigned long)(pos >> PAGE_SHIFT),
	                             GFP_KERNEL);
	if (!folio)
		return -ENOMEM;
	*pagep = folio_page(folio, 0);
	return 0;
}

static int shmem_write_end(struct file *file, struct address_space *mapping,
                           loff_t pos, unsigned len, unsigned copied,
                           struct page *page, void *fsdata)
{
	(void)file; (void)mapping; (void)pos; (void)len; (void)page; (void)fsdata;
	/* Nothing to mark: the page is the storage, not a cache of it. */
	return (int)copied;
}

/* Writing a page back to its backing store. There is none — these pages are the
 * store — so this reports success without doing anything, which is what leaves
 * the data where it already is. */
static int shmem_writepage(struct page *page, struct writeback_control *wbc)
{
	(void)page; (void)wbc;
	return 0;
}

static const struct address_space_operations lkpi_shmem_aops = {
	.writepage = shmem_writepage,
	.write_begin = shmem_write_begin,
	.write_end = shmem_write_end,
};

static const struct file_operations lkpi_shmem_fops = {
	.release = shmem_file_release,
};

struct file *shmem_file_setup(const char *name, loff_t size,
                              unsigned long flags)
{
	struct lkpi_shmem *sh;
	usize npages;

	(void)name;
	(void)flags;
	if (size < 0)
		return ERR_PTR(-EINVAL);

	npages = (usize)((size + PAGE_SIZE - 1) >> PAGE_SHIFT);
	sh = lkpi_kmalloc(sizeof(*sh), GFP_KERNEL | __GFP_ZERO);
	if (!sh)
		return ERR_PTR(-ENOMEM);
	if (npages) {
		sh->pages = lkpi_pagevec_alloc(npages);
		if (!sh->pages) {
			lkpi_kfree(sh);
			return ERR_PTR(-ENOMEM);
		}
	}
	sh->npages = npages;

	sh->inode.i_size = size;
	sh->inode.i_mapping = &sh->mapping;
	sh->mapping.host = &sh->inode;
	sh->mapping.a_ops = &lkpi_shmem_aops;
	sh->mapping.gfp_mask = GFP_KERNEL;

	sh->file.f_inode = &sh->inode;
	sh->file.f_mapping = &sh->mapping;
	sh->file.f_op = &lkpi_shmem_fops;
	sh->file.f_mode = FMODE_READ | FMODE_WRITE;
	atomic64_set(&sh->file.f_count, 1);
	return &sh->file;
}

struct file *shmem_file_setup_with_mnt(struct vfsmount *mnt, const char *name,
                                       loff_t size, unsigned long flags)
{
	/* The mount selects which tmpfs instance the inode comes from. There is
	 * only one source of pages here, so it selects nothing. */
	(void)mnt;
	return shmem_file_setup(name, size, flags);
}

struct folio *shmem_read_folio_gfp(struct address_space *mapping,
                                   unsigned long index, gfp_t gfp)
{
	struct lkpi_shmem *sh = shmem_from_mapping(mapping);

	(void)gfp;
	if (!sh || index >= sh->npages)
		return 0;
	if (!sh->pages[index].phys) {
		if (!lkpi_pagevec_populate(sh->pages, index))
			return 0;
		/* Zeroed on first use: a GEM object must not hand a process the
		 * contents of whatever last owned the frame. */
		__builtin_memset(page_address(&sh->pages[index]), 0, PAGE_SIZE);
	}
	/* struct folio starts with its struct page — see <linux/mm.h> — so one
	 * page is its own folio. */
	return page_folio(&sh->pages[index]);
}

void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end)
{
	struct lkpi_shmem *sh;
	usize first, last;

	if (!inode || !inode->i_mapping)
		return;
	sh = shmem_from_mapping(inode->i_mapping);
	if (!sh)
		return;
	first = (usize)(start >> PAGE_SHIFT);
	last = (end < 0) ? (sh->npages ? sh->npages - 1 : 0)
	                 : (usize)(end >> PAGE_SHIFT);
	shmem_drop_range(sh, first, last);
}
