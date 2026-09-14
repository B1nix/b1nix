/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the generic file operations.
 *
 * The parts of read, write and seek that are the same for every filesystem:
 * checking a write against the file's limits, walking the page cache for a
 * buffered read, and the seek arithmetic. A filesystem installs these in its
 * `file_operations` and supplies only what is specific to it.
 *
 * `generic_file_read_iter` is the one that matters for a mount to be useful: it
 * is how the contents of a file reach a reader, through the page cache and the
 * filesystem's own `read_folio`.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/uio.h>
#include <linux/slab.h>
#include <linux/writeback.h>
#include <linux/mount.h>
#include <linux/backing-dev.h>
#include <linux/limits.h>

/* ── seeking ────────────────────────────────────────────────────── */

/*
 * Set the file position, rejecting the ones that cannot be represented.
 *
 * A negative offset is refused rather than clamped: a caller that seeks to -1
 * and then writes would otherwise write at zero, which is a data corruption
 * dressed as a seek.
 */
loff_t vfs_setpos(struct file *file, loff_t offset, loff_t maxsize)
{
	if (offset < 0 && !(file->f_mode & FMODE_UNSIGNED_OFFSET))
		return -EINVAL;
	if (offset > maxsize)
		return -EINVAL;
	if (offset != file->f_pos)
		file->f_pos = offset;
	return offset;
}

loff_t generic_file_llseek_size(struct file *file, loff_t offset, int whence,
                                loff_t maxsize, loff_t eof)
{
	switch (whence) {
	case SEEK_END:
		offset += eof;
		break;
	case SEEK_CUR:
		/*
		 * A zero-offset SEEK_CUR is "where am I", and must not be turned
		 * into a set: two threads sharing a descriptor would race and one
		 * would get the other's position.
		 */
		if (offset == 0)
			return file->f_pos;
		offset += file->f_pos;
		break;
	case SEEK_DATA:
		/* Without an extent map to consult, every offset inside the file is
		 * data. That is a legal answer — SEEK_DATA may report less sparseness
		 * than the file has — where claiming a hole would not be. */
		if (offset >= eof)
			return -ENXIO;
		break;
	case SEEK_HOLE:
		/* Symmetrically: the only hole is at end of file. */
		if (offset >= eof)
			return -ENXIO;
		offset = eof;
		break;
	default:
		break;
	}
	return vfs_setpos(file, offset, maxsize);
}

loff_t generic_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;

	return generic_file_llseek_size(file, offset, whence,
	                                inode->i_sb->s_maxbytes,
	                                i_size_read(inode));
}

loff_t default_llseek(struct file *file, loff_t offset, int whence)
{
	return generic_file_llseek(file, offset, whence);
}

loff_t no_seek_end_llseek(struct file *file, loff_t offset, int whence)
{
	if (whence == SEEK_END)
		return -EINVAL;
	return generic_file_llseek(file, offset, whence);
}

/* ── opening ────────────────────────────────────────────────────── */

int generic_file_open(struct inode *inode, struct file *filp)
{
	/*
	 * The only generic check: a file too large for the caller's offset type.
	 * Everything else about an open is the VFS's business above this layer.
	 */
	if (!(filp->f_flags & O_LARGEFILE) &&
	    i_size_read(inode) > (loff_t)MAX_NON_LFS)
		return -EOVERFLOW;
	return 0;
}

void file_ra_state_init(struct file_ra_state *ra, struct address_space *mapping)
{
	memset(ra, 0, sizeof(*ra));
	ra->ra_pages = mapping && mapping->host && mapping->host->i_sb &&
	                       mapping->host->i_sb->s_bdi
	                   ? mapping->host->i_sb->s_bdi->ra_pages
	                   : 32;
	ra->prev_pos = -1;
}

void init_sync_kiocb(struct kiocb *kiocb, struct file *filp)
{
	memset(kiocb, 0, sizeof(*kiocb));
	kiocb->ki_filp = filp;
	/* NULL ki_complete is what `is_sync_kiocb` tests: this I/O is finished
	 * when the call returns, and nobody will be told about it later. */
	kiocb->ki_complete = NULL;
	kiocb->ki_flags = 0;
	kiocb->ki_pos = filp ? filp->f_pos : 0;
}

int kiocb_set_rw_flags(struct kiocb *ki, rwf_t flags, int rw_type)
{
	int kiocb_flags = 0;

	(void)rw_type;
	/* RWF_ATOMIC and RWF_DONTCACHE are refused: no file here advertises
	 * FMODE_CAN_ATOMIC_WRITE or FOP_DONTCACHE support through this path. */
	if (flags & ~(RWF_HIPRI | RWF_DSYNC | RWF_SYNC | RWF_NOWAIT | RWF_APPEND))
		return -EOPNOTSUPP;
	if (flags & RWF_NOWAIT)
		kiocb_flags |= IOCB_NOWAIT;
	if (flags & RWF_HIPRI)
		kiocb_flags |= IOCB_HIPRI;
	if (flags & RWF_DSYNC)
		kiocb_flags |= IOCB_DSYNC;
	if (flags & RWF_SYNC)
		kiocb_flags |= IOCB_DSYNC | IOCB_SYNC;
	if (flags & RWF_APPEND)
		kiocb_flags |= IOCB_APPEND;
	ki->ki_flags |= kiocb_flags;
	return 0;
}

/* ── reading ────────────────────────────────────────────────────── */

/*
 * A buffered read, through the page cache.
 *
 * For each page of the range: find it, and if it is not up to date ask the
 * filesystem to fill it, then copy out. A short read at end of file is not an
 * error — it is how the caller learns where the file ends.
 */
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter,
                     ssize_t already_read)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	loff_t pos = iocb->ki_pos;
	loff_t isize = i_size_read(inode);
	size_t want = iov_iter_count(iter);
	ssize_t copied = 0;

	if (pos >= isize)
		return already_read ? already_read : 0;
	if ((loff_t)want > isize - pos)
		want = (size_t)(isize - pos);

	while (want) {
		pgoff_t index = (pgoff_t)(pos >> PAGE_SHIFT);
		size_t offset = (size_t)(pos & (PAGE_SIZE - 1));
		size_t chunk = PAGE_SIZE - offset;
		struct folio *folio;
		size_t moved;

		if (chunk > want)
			chunk = want;

		/* A miss reads the rest of the request ahead in one batch instead of
		 * a page at a time -- see page_cache_ra_unbounded. */
		if (!xa_load(&mapping->i_pages, index))
			page_cache_sync_readahead(mapping, &file->f_ra, file, index,
			                          (unsigned long)((offset + want +
			                                           PAGE_SIZE - 1) >>
			                                          PAGE_SHIFT));

		folio = read_mapping_folio(mapping, index, file);
		if (IS_ERR(folio)) {
			if (copied)
				break;   /* report what was read; the error next time */
			return PTR_ERR(folio);
		}

		moved = copy_folio_to_iter(folio, offset, chunk, iter);
		folio_put(folio);
		if (!moved)
			break;       /* the destination faulted or filled up */

		pos += (loff_t)moved;
		copied += (ssize_t)moved;
		want -= moved;
	}

	iocb->ki_pos = pos;
	file_accessed(file);
	return already_read + copied;
}

ssize_t generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	if (!iov_iter_count(iter))
		return 0;
	if (iocb->ki_flags & IOCB_DIRECT) {
		/*
		 * Direct I/O goes through the filesystem's own direct_IO, and a
		 * filesystem without one has no way to bypass the cache — so the
		 * read falls back to the buffered path rather than failing. That is
		 * upstream's behaviour too, and it is why O_DIRECT is a hint.
		 */
		struct address_space *mapping = iocb->ki_filp->f_mapping;

		if (mapping->a_ops && mapping->a_ops->direct_IO) {
			ssize_t ret = mapping->a_ops->direct_IO(iocb, iter);

			if (ret >= 0 || ret != -ENOTBLK)
				return ret;
		}
	}
	return filemap_read(iocb, iter, 0);
}

void file_accessed(struct file *file)
{
	/*
	 * Through touch_atime, which is where the refusals live: a read-only
	 * mount must not update an access time. Calling inode_update_time
	 * directly skipped that check, and the atime update dirtied an inode,
	 * which made btrfs join a transaction on a filesystem whose every block
	 * group is read-only — "Transaction aborted (error -28)" on a mount that
	 * had only read a directory.
	 */
	if (!(file->f_flags & O_NOATIME))
		touch_atime(&file->f_path);
}

/* ── writing ────────────────────────────────────────────────────── */

/*
 * The checks every write shares: append mode, the file size limit, and the
 * filesystem's maximum.
 *
 * It also TRUNCATES the iterator when the write would run past the limit,
 * which is why it takes it by pointer — a caller that ignored that would write
 * past the maximum the filesystem can represent.
 */
int generic_write_checks(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	loff_t count = (loff_t)iov_iter_count(from);
	loff_t limit;

	if (IS_SWAPFILE(inode))
		return -ETXTBSY;
	if (!count)
		return 0;

	if (iocb->ki_flags & IOCB_APPEND)
		iocb->ki_pos = i_size_read(inode);
	if (iocb->ki_pos < 0)
		return -EINVAL;

	limit = inode->i_sb->s_maxbytes;
	if (iocb->ki_pos >= limit)
		return -EFBIG;
	if (count > limit - iocb->ki_pos) {
		count = limit - iocb->ki_pos;
		iov_iter_truncate(from, (u64)count);
	}
	return (int)count;
}

ssize_t generic_write_checks_count(struct kiocb *iocb, loff_t *count)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	loff_t limit = inode->i_sb->s_maxbytes;

	if (IS_SWAPFILE(inode))
		return -ETXTBSY;
	if (iocb->ki_flags & IOCB_APPEND)
		iocb->ki_pos = i_size_read(inode);
	if (iocb->ki_pos < 0)
		return -EINVAL;
	if (iocb->ki_pos >= limit)
		return -EFBIG;
	if (*count > limit - iocb->ki_pos)
		*count = limit - iocb->ki_pos;
	return 0;
}

/*
 * A buffered write, through the filesystem's write_begin / write_end.
 *
 * The pair is what lets a filesystem allocate blocks and journal the change
 * around the copy: write_begin hands back a locked page ready to be written
 * into, write_end records how much actually was. Skipping either leaves the
 * page dirty with no allocation behind it.
 */
ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *i)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	loff_t pos = iocb->ki_pos;
	ssize_t written = 0;

	if (!a_ops || !a_ops->write_begin || !a_ops->write_end)
		return -EOPNOTSUPP;

	while (iov_iter_count(i)) {
		size_t offset = (size_t)(pos & (PAGE_SIZE - 1));
		size_t bytes = PAGE_SIZE - offset;
		struct folio *folio = NULL;
		void *fsdata = NULL;
		size_t copied;
		int status;

		if (bytes > iov_iter_count(i))
			bytes = iov_iter_count(i);

		status = a_ops->write_begin(iocb, mapping, pos, (unsigned)bytes,
		                            &folio, &fsdata);
		if (status < 0) {
			if (written)
				break;
			return status;
		}

		copied = copy_page_from_iter_atomic(folio_page(folio, 0), offset,
		                                    bytes, i);

		status = a_ops->write_end(iocb, mapping, pos, (unsigned)bytes,
		                          (unsigned)copied, folio, fsdata);
		if (status < 0) {
			if (written)
				break;
			return status;
		}
		/*
		 * write_end reports how much it accepted, which can be less than was
		 * copied — it is the filesystem's answer, not the copy's, and using
		 * the copy's count here would advance past bytes it did not record.
		 */
		copied = (size_t)status;
		if (!copied)
			break;

		pos += (loff_t)copied;
		written += (ssize_t)copied;
	}

	iocb->ki_pos = pos;
	return written ? written : 0;
}

ssize_t __generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	int count = generic_write_checks(iocb, from);

	if (count <= 0)
		return count;
	return generic_perform_write(iocb, from);
}

ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	inode_lock(inode);
	ret = __generic_file_write_iter(iocb, from);
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

/*
 * Flush a synchronous write before returning.
 *
 * Only for the descriptors that asked (O_SYNC / O_DSYNC): doing it for every
 * write would make a buffered write synchronous, which is a correctness-safe
 * change that costs an order of magnitude.
 */
ssize_t generic_write_sync(struct kiocb *iocb, ssize_t count)
{
	if (count > 0 && (iocb->ki_flags & IOCB_DSYNC)) {
		struct file *file = iocb->ki_filp;
		int ret = vfs_fsync_range(file, iocb->ki_pos - count,
		                          iocb->ki_pos - 1,
		                          (iocb->ki_flags & IOCB_SYNC) ? 0 : 1);

		if (ret)
			return ret;
	}
	return count;
}

int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync)
{
	if (!file->f_op || !file->f_op->fsync)
		return -EINVAL;
	return file->f_op->fsync(file, start, end, datasync);
}

int generic_file_fsync(struct file *file, loff_t start, loff_t end,
                       int datasync)
{
	struct inode *inode = file->f_mapping->host;
	int ret;

	(void)datasync;
	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		return ret;
	return sync_inode_metadata(inode, 1);
}

/* ── directories ────────────────────────────────────────────────── */

/*
 * Reading a directory as if it were a file.
 *
 * -EISDIR rather than leaving `read` NULL: a NULL gives EINVAL, and userspace
 * distinguishes the two — `cat` on a directory is expected to say "Is a
 * directory".
 */
ssize_t generic_read_dir(struct file *filp, char __user *buf, size_t siz,
                         loff_t *ppos)
{
	(void)filp;
	(void)buf;
	(void)siz;
	(void)ppos;
	return -EISDIR;
}

bool dir_emit_dot(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);

	return ctx->actor(ctx, ".", 1, ctx->pos, inode->i_ino, DT_DIR);
}

bool dir_emit_dotdot(struct file *file, struct dir_context *ctx)
{
	struct dentry *parent = file->f_path.dentry
	                            ? file->f_path.dentry->d_parent
	                            : NULL;
	u64 ino = parent && parent->d_inode ? parent->d_inode->i_ino
	                                    : file_inode(file)->i_ino;

	return ctx->actor(ctx, "..", 2, ctx->pos, ino, DT_DIR);
}

bool dir_emit_dots(struct file *file, struct dir_context *ctx)
{
	/*
	 * Position 0 is ".", 1 is "..", and everything after is the filesystem's.
	 * The position is advanced only when the entry was accepted, so a full
	 * buffer resumes at the same entry rather than skipping it.
	 */
	if (ctx->pos == 0) {
		if (!dir_emit_dot(file, ctx))
			return false;
		ctx->pos = 1;
	}
	if (ctx->pos == 1) {
		if (!dir_emit_dotdot(file, ctx))
			return false;
		ctx->pos = 2;
	}
	return true;
}

int dir_relax(struct inode *inode)
{
	/*
	 * Drop and retake the directory lock so a long readdir does not hold it
	 * across the whole directory. Everything the caller cached across the gap
	 * is stale afterwards, which is why it returns whether the directory is
	 * still there.
	 */
	inode_unlock(inode);
	inode_lock(inode);
	return !IS_DEADDIR(inode);
}

int dir_relax_shared(struct inode *inode)
{
	inode_unlock_shared(inode);
	inode_lock_shared(inode);
	return !IS_DEADDIR(inode);
}

/* ── mount write references ─────────────────────────────────────── */

/*
 * A write reference on the mount, so it cannot be remounted read-only while
 * the operation is in flight.
 *
 * b1nix's mounts are managed by its own VFS above this layer, and an imported
 * filesystem never sees a vfsmount that layer created — the `f_path.mnt` on a
 * file it is handed is NULL. So these are structural, and the read-only check
 * they would perform is done by b1nix's VFS before the call arrives.
 */
int mnt_want_write(struct vfsmount *mnt) { (void)mnt; return 0; }
void mnt_drop_write(struct vfsmount *mnt) { (void)mnt; }
int mnt_want_write_file(struct file *file) { (void)file; return 0; }
void mnt_drop_write_file(struct file *file) { (void)file; }
int __mnt_want_write(struct vfsmount *mnt) { (void)mnt; return 0; }
void __mnt_drop_write(struct vfsmount *mnt) { (void)mnt; }
int __mnt_want_write_file(struct file *file) { (void)file; return 0; }
void __mnt_drop_write_file(struct file *file) { (void)file; }
void mntput(struct vfsmount *mnt) { (void)mnt; }

struct mnt_idmap nop_mnt_idmap;

struct mnt_idmap *file_mnt_idmap(struct file *file)
{
	(void)file;
	return &nop_mnt_idmap;
}

struct mnt_idmap *mnt_idmap(const struct vfsmount *mnt)
{
	(void)mnt;
	return &nop_mnt_idmap;
}

/* ── file references ────────────────────────────────────────────── */

void file_start_write(struct file *file)
{
	struct inode *inode = file_inode(file);

	if (inode && inode->i_sb && S_ISREG(inode->i_mode))
		sb_start_write(inode->i_sb);
}

bool file_start_write_trylock(struct file *file)
{
	struct inode *inode = file_inode(file);

	if (!inode || !inode->i_sb || !S_ISREG(inode->i_mode))
		return true;
	return sb_start_write_trylock(inode->i_sb);
}

void file_end_write(struct file *file)
{
	struct inode *inode = file_inode(file);

	if (inode && inode->i_sb && S_ISREG(inode->i_mode))
		sb_end_write(inode->i_sb);
}

int get_write_access(struct inode *inode)
{
	/* Refused while the file is being executed: that is what ETXTBSY is. */
	if (atomic_read(&inode->i_writecount) < 0)
		return -ETXTBSY;
	atomic_inc(&inode->i_writecount);
	return 0;
}

void put_write_access(struct inode *inode)
{
	atomic_dec(&inode->i_writecount);
}

int deny_write_access(struct file *file)
{
	struct inode *inode = file_inode(file);

	if (atomic_read(&inode->i_writecount) > 0)
		return -ETXTBSY;
	atomic_dec(&inode->i_writecount);
	return 0;
}

void allow_write_access(struct file *file)
{
	if (file)
		atomic_inc(&file_inode(file)->i_writecount);
}

/* ── the remaining generic entry points ─────────────────────────── */

int generic_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	/*
	 * Declared and refused: a file-backed mapping needs the fault path to
	 * reach this page cache, and b1nix's fault handler does not yet. Refusing
	 * is what makes that visible; a success would hand back a mapping whose
	 * first touch faults into nothing.
	 */
	(void)file;
	(void)vma;
	return -ENODEV;
}

int generic_file_readonly_mmap(struct file *file, struct vm_area_struct *vma)
{
	return generic_file_mmap(file, vma);
}

ssize_t generic_file_splice_read(struct file *in, loff_t *ppos,
                                 struct pipe_inode_info *pipe, size_t len,
                                 unsigned int flags)
{
	(void)in; (void)ppos; (void)pipe; (void)len; (void)flags;
	return -EINVAL;
}

ssize_t filemap_splice_read(struct file *in, loff_t *ppos,
                            struct pipe_inode_info *pipe, size_t len,
                            unsigned int flags)
{
	return generic_file_splice_read(in, ppos, pipe, len, flags);
}

ssize_t iter_file_splice_write(struct pipe_inode_info *pipe, struct file *out,
                               loff_t *ppos, size_t len, unsigned int flags)
{
	(void)pipe; (void)out; (void)ppos; (void)len; (void)flags;
	return -EINVAL;
}

ssize_t kernel_write(struct file *file, const void *buf, size_t count,
                     loff_t *pos)
{
	struct kvec kv = { .iov_base = (void *)buf, .iov_len = count };
	struct iov_iter iter;
	struct kiocb kiocb;
	ssize_t ret;

	if (!file->f_op || !file->f_op->write_iter)
		return -EINVAL;
	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = pos ? *pos : 0;
	iov_iter_kvec(&iter, ITER_SOURCE, &kv, 1, count);
	ret = file->f_op->write_iter(&kiocb, &iter);
	if (ret > 0 && pos)
		*pos = kiocb.ki_pos;
	return ret;
}

int rw_verify_area(int read_write, struct file *file, const loff_t *ppos,
                   size_t count)
{
	(void)read_write;
	(void)file;
	if (ppos && *ppos < 0)
		return -EINVAL;
	if (count > (size_t)MAX_RW_COUNT)
		return (int)MAX_RW_COUNT;
	return (int)count;
}

loff_t generic_remap_file_range_prep(struct file *file_in, loff_t pos_in,
                                     struct file *file_out, loff_t pos_out,
                                     loff_t *len, unsigned int remap_flags)
{
	(void)file_in; (void)pos_in; (void)file_out; (void)pos_out;
	(void)remap_flags;
	/* No clamping against the files' sizes yet: a filesystem that supports
	 * reflink does its own bounds checking, and btrfs does. */
	return *len;
}

int finish_open_simple(struct file *file, int error)
{
	(void)file;
	return error;
}

/* Returns an ERROR, not the block: the block goes back through the pointer.
 * jbd2 reads the return value as errno — `err = bmap(inode, &blocknr); if (err
 * || !blocknr)` — so handing it the block number made every successful lookup
 * read as a failure, and ext4 could not find its journal superblock. */
int bmap(struct inode *inode, sector_t *block)
{
	if (!inode || !block)
		return -EINVAL;
	if (!inode->i_mapping || !inode->i_mapping->a_ops ||
	    !inode->i_mapping->a_ops->bmap)
		return -EINVAL;
	*block = inode->i_mapping->a_ops->bmap(inode->i_mapping, *block);
	return 0;
}

int kiocb_write_and_wait(struct kiocb *iocb, size_t count)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	loff_t pos = iocb->ki_pos;

	return filemap_write_and_wait_range(mapping, pos, pos + (loff_t)count - 1);
}

int kiocb_invalidate_pages(struct kiocb *iocb, size_t count)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	loff_t pos = iocb->ki_pos;

	/*
	 * A direct write must drop the cached copies of what it is about to
	 * overwrite, or a later read returns the stale page. Writing them back
	 * first is what keeps a dirty page from being lost rather than merely
	 * stale.
	 */
	int ret = filemap_write_and_wait_range(mapping, pos,
	                                       pos + (loff_t)count - 1);

	if (ret)
		return ret;
	return invalidate_inode_pages2_range(mapping, pos >> PAGE_SHIFT,
	                                     (pos + (loff_t)count - 1) >>
	                                         PAGE_SHIFT);
}

void kiocb_invalidate_post_direct_write(struct kiocb *iocb, size_t count)
{
	struct address_space *mapping = iocb->ki_filp->f_mapping;
	loff_t pos = iocb->ki_pos;

	invalidate_inode_pages2_range(mapping, pos >> PAGE_SHIFT,
	                              (pos + (loff_t)count - 1) >> PAGE_SHIFT);
}

int iocb_bio_iopoll(struct kiocb *kiocb, struct io_comp_batch *iob,
                    unsigned int flags)
{
	(void)kiocb; (void)iob; (void)flags;
	/* Nothing to poll: submissions complete before submit_bio returns. */
	return 0;
}

ssize_t noop_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	(void)iocb; (void)iter;
	return -EINVAL;
}

int filemap_fault(struct vm_fault *vmf)
{
	(void)vmf;
	/* See generic_file_mmap: file-backed mappings are not wired to this page
	 * cache yet. */
	return VM_FAULT_SIGBUS;
}

/* Map nothing ahead of the fault: the fault handler itself answers. */
vm_fault_t filemap_map_pages(struct vm_fault *vmf, pgoff_t start, pgoff_t end)
{
	(void)vmf; (void)start; (void)end;
	return 0;
}

int filemap_page_mkwrite(struct vm_fault *vmf)
{
	(void)vmf;
	return VM_FAULT_SIGBUS;
}
