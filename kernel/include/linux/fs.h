/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FS_H
#define LKPI_LINUX_FS_H

#include <linux/kdev_t.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/uidgid.h>
#include <linux/percpu-rwsem.h>
#include <linux/percpu_counter.h>
#include <linux/semaphore.h>
/*
 * Quotas: the imported subsystem's own headers when fs/quota is in the build,
 * and the "absent" shape otherwise. The shim's copy lives under lkpi/ rather
 * than linux/ on purpose — kernel/include comes first on the include path, so
 * a linux/quota.h of ours would shadow upstream's for the imported files while
 * the rest of the kernel kept using it, and struct super_block would have two
 * different layouts in one link.
 */
#ifdef B1NIX_FS_IMPORT
#include <linux/quota.h>
#else
#include <lkpi/quota-absent.h>
#endif
#include <linux/xarray.h>
#include <linux/errno.h>
/* The writeback-error sequence helpers, used through a file's f_wb_err. */
#include <linux/errseq.h>
/*
 * Both are dependencies of declarations in this header — `iov_iter` for the
 * read/write paths, `writeback_control` for two of the operations vectors — and
 * both are how imported code reaches ITER_DEST, UIO_FASTIOV and WB_SYNC_ALL
 * from files that include only <linux/fs.h>.
 */
#include <linux/uio.h>
#include <linux/writeback.h>
/* struct workqueue_struct is a member of the superblock below, and the WQ_*
 * flags are how a filesystem creates its own — reached from files that include
 * only this header. */
#include <linux/workqueue.h>
/* Both are upstream's transitive includes here, and the quota code relies on
 * them arriving this way: it uses srcu to protect an inode's dquot pointers
 * and vfs_pressure_ratio to size its shrinker. */
#include <linux/srcu.h>
#include <linux/dcache.h>
/* pfn_t, for the DAX interfaces named in <linux/dax.h>. */
#include <linux/pfn_t.h>
/* `enum migrate_mode` appears in address_space_operations below, and both
 * filesystems name the migrate helpers in their own operations tables from
 * files that include neither <linux/migrate.h> nor anything reaching it. */
#include <linux/migrate.h>
/* audit_inode_child, called from the rename and unlink paths. */
#include <linux/audit.h>
/* The ABI half: the ioctl argument structures and the FS_*_FL attribute flags.
 * Upstream's <linux/fs.h> includes its uapi counterpart for the same reason —
 * a filesystem's ioctl handler needs both halves and includes only this one. */
#include <uapi/linux/fs.h>
/* XATTR_NAME_MAX and the attribute namespace. fs/internal.h sizes a buffer with
 * it while including only <linux/fs.h>, and upstream reaches it the same way. */
#include <uapi/linux/xattr.h>
/* __kernel_fsid_t, for uuid_to_fsid below. */
#include <linux/statfs.h>
#include <linux/time64.h>
#include <linux/list_bl.h>
#include <linux/lockref.h>
#include <linux/seqlock.h>
#include <linux/path.h>
#include <linux/cred.h>
#include <linux/posix_acl.h>
/* struct shrinker, embedded in the superblock below. */
#include <linux/shrinker.h>

/*
 * The VFS object model.
 *
 * This is the largest single piece of the shim and the one everything else
 * hangs off: a filesystem IS an implementation of these structures. The shapes
 * are upstream's, member for member, because imported code reads and writes
 * them directly — `inode->i_size`, `sb->s_fs_info`, `dentry->d_name.name` — and
 * a rearranged structure compiles and then means something else.
 *
 * What is here and what is not:
 *
 *   - The objects (super_block, inode, dentry, file, address_space) are real
 *     and complete enough that both filesystems can be built on them.
 *   - The operations vectors are upstream's, so a filesystem's tables fill in
 *     the members it means to.
 *   - The *implementation* — allocating an inode, hashing it, walking a path,
 *     reading a page — lives in kernel/lkpi/vfs*.c, and the bridge from b1nix's
 *     own VFS into these objects is what makes any of it reachable.
 *
 * Two b1nix-specific notes that are easy to get wrong later:
 *
 *   - `i_rwsem` is the inode lock every directory operation and every write
 *     takes. It is a real rwsem here, not a mutex, because a filesystem takes
 *     it shared for a read and exclusive for a write and both happen at once.
 *   - `i_lock` is a spinlock guarding the inode's own counters (i_state,
 *     i_count, i_blocks). It is NOT the same lock and may not be held across a
 *     sleep — b1nix's rules on that are stricter than Linux's, and the ordering
 *     rule in CLAUDE.md (block cache lock, then inode lock) applies to it.
 */

struct super_block;
struct inode;
struct dentry;
struct file;
struct seq_file;
struct file_operations;
struct inode_operations;
struct super_operations;
struct address_space;
struct address_space_operations;
struct module;
struct vm_area_struct;
struct vm_fault;
struct writeback_control;
struct readahead_control;
struct kiocb;
struct iov_iter;
struct page;
struct folio;
struct block_device;
struct backing_dev_info;
struct export_operations;
struct xattr_handler;
struct fs_context;
struct fs_parameter_spec;
struct iomap;
struct unicode_map;
struct fscrypt_operations;
struct fsverity_operations;
struct mnt_idmap;
struct user_namespace;
struct pipe_inode_info;
struct fiemap_extent_info;
struct swap_info_struct;
struct file_lock;
struct poll_table_struct;
typedef struct poll_table_struct poll_table;
/* Declared here because prototypes above their definitions name them, and a
 * struct first seen inside a parameter list is local to that prototype. */
struct file_ra_state;
struct kiocb;
struct folio_batch;
struct io_comp_batch;
struct io_uring_cmd;
struct shrink_control;
struct mtd_info;
struct workqueue_struct;
struct kstatfs;
struct fileattr;
struct posix_acl;
struct dquot_operations;
struct quotactl_ops;
/* The identity of whoever owns a POSIX lock: the file table it was taken
 * through, as an opaque pointer. It is a pointer rather than a pid because a
 * lock survives a fork and belongs to the opener, not to a task. */
typedef void *fl_owner_t;

/*
 * A writeback error, recorded as a sequence rather than a plain errno.
 *
 * The point of the encoding is that each opener sees a given error exactly
 * once: the low bits are the errno, the high bits a counter, and a reader
 * compares its own saved value against the mapping's. A plain errno cannot do
 * that — it either sticks forever, so every later fsync fails, or it is cleared
 * by the first reader, so everybody else never learns.
 */
#ifndef LKPI_ERRSEQ_T_DEFINED
#define LKPI_ERRSEQ_T_DEFINED
typedef u32 errseq_t;
#endif

/* ── small value types ──────────────────────────────────────────── */

/*
 * A name, with its length and hash precomputed.
 *
 * The hash is not optional decoration: a dentry cache lookup compares hashes
 * before bytes, so a qstr built without one never matches anything. Both
 * filesystems construct qstrs by hand in places, which is why QSTR_INIT exists
 * and why the hash is a separate step (`d_hash` fills it).
 */
struct qstr {
	union {
		struct {
			u32 hash;
			u32 len;
		};
		u64 hash_len;
	};
	const unsigned char *name;
};

#define QSTR_INIT(n, l) { { { .hash = 0, .len = (l) } }, .name = (n) }
#define hashlen_hash(hashlen) ((u32)(hashlen))
#define hashlen_len(hashlen)  ((u32)((hashlen) >> 32))

/* struct timespec64 comes from <linux/time64.h>: it is a time type, not a
 * filesystem one, and two definitions of it is a redefinition error rather
 * than a merge. */

/* Attributes being changed by setattr, and which of them are valid. */
#define ATTR_MODE      (1 << 0)
#define ATTR_UID       (1 << 1)
#define ATTR_GID       (1 << 2)
#define ATTR_SIZE      (1 << 3)
#define ATTR_ATIME     (1 << 4)
#define ATTR_MTIME     (1 << 5)
#define ATTR_CTIME     (1 << 6)
#define ATTR_ATIME_SET (1 << 7)
#define ATTR_MTIME_SET (1 << 8)
#define ATTR_FORCE     (1 << 9)
#define ATTR_KILL_SUID (1 << 11)
#define ATTR_KILL_SGID (1 << 12)
#define ATTR_FILE      (1 << 13)
#define ATTR_KILL_PRIV (1 << 14)
#define ATTR_OPEN      (1 << 15)
#define ATTR_TIMES_SET (1 << 16)
#define ATTR_TOUCH     (1 << 17)

/* The idmapped-mount id types. b1nix has no idmapped mounts, so a vfsuid IS a
 * kuid — but the types stay apart, because that is the distinction that stops a
 * mount-relative id being written to disk. */
typedef kuid_t vfsuid_t;
typedef kgid_t vfsgid_t;
static inline kuid_t vfsuid_into_kuid(vfsuid_t vfsuid) { return vfsuid; }
static inline kgid_t vfsgid_into_kgid(vfsgid_t vfsgid) { return vfsgid; }

struct iattr {
	unsigned int ia_valid;
	umode_t ia_mode;
	kuid_t ia_uid;
	kgid_t ia_gid;
	/* The same two ids as the mount sees them. With no idmapped mounts these
	 * are the same numbers; upstream's setattr paths read the vfs* pair, so
	 * both names exist and a caller filling one must fill the other. */
	vfsuid_t ia_vfsuid;
	vfsgid_t ia_vfsgid;
	loff_t ia_size;
	struct timespec64 ia_atime;
	struct timespec64 ia_mtime;
	struct timespec64 ia_ctime;
	struct file *ia_file;
};

struct kstat {
	u32 result_mask;
	umode_t mode;
	unsigned int nlink;
	u32 blksize;
	u64 attributes;
	u64 attributes_mask;
	u64 ino;
	dev_t dev;
	dev_t rdev;
	kuid_t uid;
	kgid_t gid;
	loff_t size;
	struct timespec64 atime;
	struct timespec64 mtime;
	struct timespec64 ctime;
	struct timespec64 btime;
	u64 blocks;
	u64 mnt_id;
	u32 dio_mem_align;
	u32 dio_offset_align;
	u64 change_cookie;
};

#define STATX_TYPE        0x00000001U
#define STATX_MODE        0x00000002U
#define STATX_NLINK       0x00000004U
#define STATX_UID         0x00000008U
#define STATX_GID         0x00000010U
#define STATX_ATIME       0x00000020U
#define STATX_MTIME       0x00000040U
#define STATX_CTIME       0x00000080U
#define STATX_INO         0x00000100U
#define STATX_SIZE        0x00000200U
#define STATX_BLOCKS      0x00000400U
#define STATX_BASIC_STATS 0x000007ffU
#define STATX_BTIME       0x00000800U
#define STATX_ATTR_IMMUTABLE 0x00000010
#define STATX_ATTR_APPEND    0x00000020
#define STATX_ATTR_NODUMP    0x00000040
#define STATX_ATTR_COMPRESSED 0x00000004

#define STATX_DIOALIGN    0x00002000U
#define STATX_ATTR_ENCRYPTED 0x00000800
#define STATX_ATTR_VERITY    0x00100000
#define STATX_ATTR_DAX       0x00200000

#define AT_STATX_SYNC_AS_STAT 0x0000

/*
 * A destructor to run later, for a caller that borrowed something whose
 * lifetime it does not own — a symlink body read into a buffer, most often.
 */
struct delayed_call {
	void (*fn)(void *);
	void *arg;
};

static inline void set_delayed_call(struct delayed_call *call,
                                    void (*fn)(void *), void *arg)
{
	call->fn = fn;
	call->arg = arg;
}

static inline void do_delayed_call(struct delayed_call *call)
{
	if (call->fn)
		call->fn(call->arg);
}

static inline void clear_delayed_call(struct delayed_call *call)
{
	call->fn = NULL;
}

/* ── directory reading ──────────────────────────────────────────── */

/*
 * `actor` returns false to stop the walk, and `ctx->pos` is where the next call
 * resumes. The return convention is upstream's and is the opposite of what the
 * name suggests to a fresh reader: false means "stop", not "failed".
 */
struct dir_context;
typedef bool (*filldir_t)(struct dir_context *, const char *, int, loff_t, u64,
                          unsigned);

struct dir_context {
	filldir_t actor;
	loff_t pos;
};

static inline bool dir_emit(struct dir_context *ctx, const char *name,
                            int namelen, u64 ino, unsigned type)
{
	return ctx->actor(ctx, name, namelen, ctx->pos, ino, type);
}

bool dir_emit_dot(struct file *file, struct dir_context *ctx);
bool dir_emit_dotdot(struct file *file, struct dir_context *ctx);
bool dir_emit_dots(struct file *file, struct dir_context *ctx);

/* d_type values, which go to userspace in a dirent. */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

/* ── address_space ──────────────────────────────────────────────── */

struct address_space_operations {
	int (*writepage)(struct page *page, struct writeback_control *wbc);
	int (*read_folio)(struct file *, struct folio *);
	int (*writepages)(struct address_space *, struct writeback_control *);
	bool (*dirty_folio)(struct address_space *, struct folio *);
	void (*readahead)(struct readahead_control *);
	int (*write_begin)(struct file *, struct address_space *mapping,
	                   loff_t pos, unsigned len, struct page **pagep,
	                   void **fsdata);
	int (*write_end)(struct file *, struct address_space *mapping,
	                 loff_t pos, unsigned len, unsigned copied,
	                 struct page *page, void *fsdata);
	sector_t (*bmap)(struct address_space *, sector_t);
	void (*invalidate_folio)(struct folio *, size_t offset, size_t len);
	bool (*release_folio)(struct folio *, gfp_t);
	void (*free_folio)(struct folio *folio);
	ssize_t (*direct_IO)(struct kiocb *, struct iov_iter *iter);
	int (*migrate_folio)(struct address_space *, struct folio *dst,
	                     struct folio *src, enum migrate_mode);
	int (*launder_folio)(struct folio *);
	bool (*is_partially_uptodate)(struct folio *, size_t from, size_t count);
	void (*is_dirty_writeback)(struct folio *, bool *dirty, bool *wb);
	int (*error_remove_page)(struct address_space *, struct page *);
	int (*swap_activate)(struct swap_info_struct *sis, struct file *file,
	                     sector_t *span);
	void (*swap_deactivate)(struct file *file);
	int (*swap_rw)(struct kiocb *iocb, struct iov_iter *iter);
};

/*
 * The page cache of one object.
 *
 * `i_pages` is the index — an xarray keyed by page offset — and it IS the
 * cache: a page is in this mapping if and only if it is in that array. The
 * lock protecting it is `i_pages.xa_lock` and it is taken at IRQ level in
 * places, which is why the writeback paths that hold it must not sleep.
 */
struct address_space {
	struct inode *host;
	struct xarray i_pages;
	struct rw_semaphore invalidate_lock;
	gfp_t gfp_mask;
	atomic_t i_mmap_writable;
	unsigned long nrpages;
	pgoff_t writeback_index;
	const struct address_space_operations *a_ops;
	unsigned long flags;
	errseq_t wb_err;
	spinlock_t private_lock;
	struct list_head private_list;
	void *private_data;
};

/* address_space flags */
#define AS_EIO          0
#define AS_ENOSPC       1
#define AS_MM_ALL_LOCKS 2
#define AS_UNEVICTABLE  3
#define AS_EXITING      4
#define AS_NO_WRITEBACK_TAGS 5
#define AS_LARGE_FOLIO_SUPPORT 6
#define AS_RELEASE_ALWAYS 7
#define AS_STABLE_WRITES 8

static inline void mapping_set_error(struct address_space *mapping, int error)
{
	if (likely(!error))
		return;
	/* Recorded on the mapping so a later fsync can report it even though the
	 * write that failed has long since returned. */
	mapping->wb_err = (errseq_t)(-error);
}

/* mapping_gfp_mask and friends live in <linux/pagemap.h>, where upstream keeps
 * them and where the rest of the page-cache interface is. */

/* ── inode ──────────────────────────────────────────────────────── */

/* i_state */
#define I_NEW           (1 << 3)  /* still being filled in; nobody may use it */
#define I_WILL_FREE     (1 << 4)
#define I_FREEING       (1 << 5)
#define I_CLEAR         (1 << 6)
#define I_SYNC          (1 << 7)
#define I_REFERENCED    (1 << 8)
#define I_DIRTY_SYNC    (1 << 0)
#define I_DIRTY_DATASYNC (1 << 1)
#define I_DIRTY_PAGES   (1 << 2)
#define I_DIRTY_TIME    (1 << 11)
#define I_LINKABLE      (1 << 10)
#define I_WB_SWITCH     (1 << 13)
#define I_OVL_INUSE     (1 << 14)
#define I_CREATING      (1 << 15)
#define I_DONTCACHE     (1 << 16)
#define I_SYNC_QUEUED   (1 << 17)
#define I_PINNING_FSCACHE_WB (1 << 18)

#define I_DIRTY_INODE (I_DIRTY_SYNC | I_DIRTY_DATASYNC)
#define I_DIRTY       (I_DIRTY_INODE | I_DIRTY_PAGES)
#define I_DIRTY_ALL   (I_DIRTY | I_DIRTY_TIME)

/* i_flags */
#define S_SYNC      (1 << 0)
#define S_NOATIME   (1 << 1)
#define S_APPEND    (1 << 2)
#define S_IMMUTABLE (1 << 3)
#define S_DEAD      (1 << 4)
#define S_NOQUOTA   (1 << 5)
#define S_DIRSYNC   (1 << 6)
#define S_NOCMTIME  (1 << 7)
#define S_SWAPFILE  (1 << 8)
#define S_PRIVATE   (1 << 9)
#define S_IMA       (1 << 10)
#define S_AUTOMOUNT (1 << 11)
#define S_NOSEC     (1 << 12)
#define S_DAX       (1 << 13)
#define S_ENCRYPTED (1 << 14)
#define S_CASEFOLD  (1 << 15)
#define S_VERITY    (1 << 16)
#define S_KERNEL_FILE (1 << 17)

/* i_opflags */
#define IOP_FASTPERM  0x0001
#define IOP_LOOKUP    0x0002
#define IOP_NOFOLLOW  0x0004
#define IOP_XATTR     0x0008
#define IOP_DEFAULT_READLINK 0x0010
#define IOP_MGTIME    0x0020

struct inode {
	umode_t i_mode;
	unsigned short i_opflags;
	kuid_t i_uid;
	kgid_t i_gid;
	unsigned int i_flags;

	const struct inode_operations *i_op;
	struct super_block *i_sb;
	struct address_space *i_mapping;

	unsigned long i_ino;
	/*
	 * The link count, and the reason it is a union upstream: a directory
	 * being deleted parks its "how many entries are left" count here while
	 * i_nlink is zero, and the two must not both be live at once.
	 */
	union {
		const unsigned int i_nlink;
		unsigned int __i_nlink;
	};
	dev_t i_rdev;
	loff_t i_size;
	struct timespec64 i_atime;
	struct timespec64 i_mtime;
	struct timespec64 __i_ctime;
	spinlock_t i_lock;    /* guards i_state, i_count, i_blocks, i_bytes */
	unsigned short i_bytes;
	u8 i_blkbits;         /* log2 of the filesystem block size */
	u8 i_write_hint;
	blkcnt_t i_blocks;    /* in 512-byte units, ALWAYS, whatever i_blkbits is */

	unsigned long i_state;
	struct rw_semaphore i_rwsem;

	unsigned long dirtied_when;
	unsigned long dirtied_time_when;

	struct hlist_node i_hash;
	struct list_head i_io_list;
	struct list_head i_lru;
	struct list_head i_sb_list;
	struct list_head i_wb_list;
	union {
		struct hlist_head i_dentry;
		struct rcu_head i_rcu;
	};

	atomic64_t i_version;
	atomic64_t i_sequence;
	atomic_t i_count;
	atomic_t i_dio_count;
	atomic_t i_writecount;

	union {
		const struct file_operations *i_fop;
		void (*free_inode)(struct inode *);
	};
	struct address_space i_data;
	struct list_head i_devices;

	union {
		const char *i_link;     /* fast symlink body, inline in the inode */
		unsigned int i_dir_seq;
	};

	u32 i_generation;
	void *i_private;

	struct posix_acl *i_acl;
	struct posix_acl *i_default_acl;
};

static inline void inode_set_ctime_to_ts(struct inode *inode,
                                         struct timespec64 ts)
{
	inode->__i_ctime = ts;
}

static inline struct timespec64 inode_get_ctime(const struct inode *inode)
{
	return inode->__i_ctime;
}

struct timespec64 current_time(struct inode *inode);
struct timespec64 inode_set_ctime_current(struct inode *inode);

static inline struct timespec64 inode_set_ctime(struct inode *inode,
                                                time64_t sec, long nsec)
{
	struct timespec64 ts = { .tv_sec = sec, .tv_nsec = nsec };

	inode_set_ctime_to_ts(inode, ts);
	return ts;
}

static inline loff_t i_size_read(const struct inode *inode)
{
	return inode->i_size;
}

static inline void i_size_write(struct inode *inode, loff_t i_size)
{
	inode->i_size = i_size;
}

static inline unsigned int i_blocksize(const struct inode *node)
{
	return 1U << node->i_blkbits;
}

/*
 * i_blocks and i_bytes together are the file's allocated size: whole 512-byte
 * units in the first, the remainder in the second. Adding bytes therefore has
 * to carry, and a version that only touched i_bytes would lose 512 bytes of
 * accounting every time it wrapped.
 */
void __inode_add_bytes(struct inode *inode, loff_t bytes);
void inode_add_bytes(struct inode *inode, loff_t bytes);
void __inode_sub_bytes(struct inode *inode, loff_t bytes);
void inode_sub_bytes(struct inode *inode, loff_t bytes);
loff_t inode_get_bytes(struct inode *inode);
void inode_set_bytes(struct inode *inode, loff_t bytes);

/*
 * Reading and writing an inode's owner as a plain integer.
 *
 * On disk a uid is a number; in the kernel it is a kuid_t, which exists so the
 * two cannot be confused. These are the conversion points, and they are the
 * only places a filesystem should be turning one into the other.
 */
static inline uid_t i_uid_read(const struct inode *inode)
{ return from_kuid(&init_user_ns, inode->i_uid); }
static inline gid_t i_gid_read(const struct inode *inode)
{ return from_kgid(&init_user_ns, inode->i_gid); }
static inline void i_uid_write(struct inode *inode, uid_t uid)
{ inode->i_uid = make_kuid(&init_user_ns, uid); }
static inline void i_gid_write(struct inode *inode, gid_t gid)
{ inode->i_gid = make_kgid(&init_user_ns, gid); }

/*
 * An inode whose read from disk failed.
 *
 * It is marked rather than discarded because callers already hold it: a lookup
 * that hit a corrupt inode must return something that fails every subsequent
 * operation, not a NULL that looks like "no such file".
 */
bool is_bad_inode(struct inode *inode);
void make_bad_inode(struct inode *inode);

/*
 * Direct I/O in flight against this inode.
 *
 * `inode_dio_wait` is taken before a truncate: direct I/O bypasses the page
 * cache, so nothing else would stop a read landing in pages the truncate has
 * already freed.
 */
void inode_dio_wait(struct inode *inode);
static inline void inode_dio_begin(struct inode *inode)
{ atomic_inc(&inode->i_dio_count); }
void inode_dio_end(struct inode *inode);

/* Update the file's timestamps and drop setuid, as a write must. */
int file_modified(struct file *file);
void file_ra_state_init(struct file_ra_state *ra, struct address_space *mapping);

/* Take a write reference on the mount behind a file, so it cannot be remounted
 * read-only underneath the operation. */
int mnt_want_write_file(struct file *file);
void mnt_drop_write_file(struct file *file);
int mnt_want_write(struct vfsmount *mnt);
void mnt_drop_write(struct vfsmount *mnt);

/*
 * The write-access count on an inode's file.
 *
 * `deny_write_access` is what makes a running executable's file unwritable
 * (ETXTBSY), and `get_write_access` is its opposite. The counts are on the
 * inode because the answer must be the same through every path to the file.
 */
int get_write_access(struct inode *inode);
int deny_write_access(struct file *file);
void put_write_access(struct inode *inode);
void allow_write_access(struct file *file);
static inline void i_readcount_inc(struct inode *inode)
{ atomic_inc(&inode->i_writecount); }
static inline void i_readcount_dec(struct inode *inode)
{ atomic_dec(&inode->i_writecount); }
int __mnt_want_write(struct vfsmount *mnt);
void __mnt_drop_write(struct vfsmount *mnt);
int __mnt_want_write_file(struct file *file);
void __mnt_drop_write_file(struct file *file);

/* Set on a descriptor that holds the file's write count. Distinct from
 * FMODE_WRITE, which says the descriptor may be written through: an O_PATH
 * descriptor has neither, and a write-denied executable has the second without
 * the first. */
#define FMODE_WRITER 0x800000

static inline bool vfsgid_in_group_p(vfsgid_t vfsgid) { (void)vfsgid; return true; }
struct user_namespace;
/* Back the other way: with no idmapped mounts and one user namespace, both
 * directions are the identity — but the calls stay, because they are where a
 * kernel WITH idmapping does the translation. */
static inline kuid_t from_vfsuid(struct mnt_idmap *idmap,
                                 struct user_namespace *ns, vfsuid_t vfsuid)
{ (void)idmap; (void)ns; return vfsuid_into_kuid(vfsuid); }
static inline kgid_t from_vfsgid(struct mnt_idmap *idmap,
                                 struct user_namespace *ns, vfsgid_t vfsgid)
{ (void)idmap; (void)ns; return vfsgid_into_kgid(vfsgid); }

/* Discard an inode's cached ACLs; called when an inode is set up before any
 * lookup can have cached one. */
void cache_no_acl(struct inode *inode);

/* The largest offset that fits in a signed loff_t. Distinct from
 * MAX_LFS_FILESIZE, which is what a given filesystem allows. */
#define OFFSET_MAX ((loff_t)LLONG_MAX)

void set_nlink(struct inode *inode, unsigned int nlink);

/* The namespace an inode's ids are expressed in. b1nix has exactly one. */
static inline struct user_namespace *i_user_ns(const struct inode *inode)
{ (void)inode; extern struct user_namespace init_user_ns; return &init_user_ns; }

/* An inode's size in bytes, read with i_lock already held. inode_get_bytes
 * takes the lock; this one is for callers that hold it, and calling the
 * locking form there would deadlock. */
loff_t __inode_get_bytes(struct inode *inode);
void inc_nlink(struct inode *inode);
void drop_nlink(struct inode *inode);
void clear_nlink(struct inode *inode);

static inline void inode_lock(struct inode *inode) { down_write(&inode->i_rwsem); }
static inline void inode_unlock(struct inode *inode) { up_write(&inode->i_rwsem); }
static inline void inode_lock_shared(struct inode *inode)
{ down_read(&inode->i_rwsem); }
static inline void inode_unlock_shared(struct inode *inode)
{ up_read(&inode->i_rwsem); }
static inline int inode_trylock(struct inode *inode)
{ return down_write_trylock(&inode->i_rwsem); }
static inline int inode_trylock_shared(struct inode *inode)
{ return down_read_trylock(&inode->i_rwsem); }
static inline int inode_is_locked(struct inode *inode)
{ return rwsem_is_locked(&inode->i_rwsem); }
static inline void inode_lock_nested(struct inode *inode, unsigned subclass)
{ down_write_nested(&inode->i_rwsem, subclass); }
static inline void inode_lock_shared_nested(struct inode *inode,
                                            unsigned subclass)
{ down_read_nested(&inode->i_rwsem, subclass); }

/* Lock ordering classes for nested inode locks. The values are upstream's;
 * b1nix has no lockdep so they document rather than enforce. */
enum inode_i_mutex_lock_class {
	I_MUTEX_NORMAL,
	I_MUTEX_PARENT,
	I_MUTEX_CHILD,
	I_MUTEX_XATTR,
	I_MUTEX_NONDIR2,
	I_MUTEX_PARENT2,
};

#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#define S_IRWXUGO (S_IRWXU | S_IRWXG | S_IRWXO)
#define S_IALLUGO (S_ISUID | S_ISGID | S_ISVTX | S_IRWXUGO)

#define IS_RDONLY(inode)    sb_rdonly((inode)->i_sb)
#define IS_SYNC(inode)      (((inode)->i_sb->s_flags & SB_SYNCHRONOUS) || \
                             ((inode)->i_flags & S_SYNC))
#define IS_DIRSYNC(inode)   (IS_SYNC(inode) || \
                             ((inode)->i_sb->s_flags & SB_DIRSYNC) || \
                             ((inode)->i_flags & S_DIRSYNC))
#define IS_MANDLOCK(inode)  0
#define IS_NOATIME(inode)   (((inode)->i_sb->s_flags & SB_NOATIME) || \
                             ((inode)->i_flags & S_NOATIME))
#define IS_APPEND(inode)    ((inode)->i_flags & S_APPEND)
#define IS_IMMUTABLE(inode) ((inode)->i_flags & S_IMMUTABLE)
#define IS_NOQUOTA(inode)   ((inode)->i_flags & S_NOQUOTA)
#define IS_SWAPFILE(inode)  ((inode)->i_flags & S_SWAPFILE)
#define IS_PRIVATE(inode)   ((inode)->i_flags & S_PRIVATE)
#define IS_DEADDIR(inode)   ((inode)->i_flags & S_DEAD)
#define IS_NOCMTIME(inode)  ((inode)->i_flags & S_NOCMTIME)
#define IS_AUTOMOUNT(inode) ((inode)->i_flags & S_AUTOMOUNT)
#define IS_DAX(inode)       0
#define IS_ENCRYPTED(inode) ((inode)->i_flags & S_ENCRYPTED)
#define IS_CASEFOLDED(inode) ((inode)->i_flags & S_CASEFOLD)
#define IS_VERITY(inode)    ((inode)->i_flags & S_VERITY)
/* The superblock keeps a change counter for this inode. Only then is i_version
 * maintained — an unconditional increment would dirty an inode on every write
 * for a filesystem that does not store it. */
#define IS_I_VERSION(inode) ((inode)->i_sb->s_flags & SB_I_VERSION)
/* Nothing to strip on write: no setuid or setgid bit, no security xattr. It is
 * a cache of a fact the filesystem established, and it is why a write can skip
 * the whole remove-privileges path. */
#define IS_NOSEC(inode)     ((inode)->i_flags & S_NOSEC)

/* ── dentry ─────────────────────────────────────────────────────── */

struct dentry_operations {
	int (*d_revalidate)(struct dentry *, unsigned int);
	int (*d_weak_revalidate)(struct dentry *, unsigned int);
	int (*d_hash)(const struct dentry *, struct qstr *);
	int (*d_compare)(const struct dentry *, unsigned int, const char *,
	                 const struct qstr *);
	int (*d_delete)(const struct dentry *);
	int (*d_init)(struct dentry *);
	void (*d_release)(struct dentry *);
	void (*d_prune)(struct dentry *);
	void (*d_iput)(struct dentry *, struct inode *);
	char *(*d_dname)(struct dentry *, char *, int);
	struct vfsmount *(*d_automount)(struct path *);
	int (*d_manage)(const struct path *, bool);
	struct dentry *(*d_real)(struct dentry *, const struct inode *);
};

/*
 * How long a name can be before the dentry needs a separate allocation for it.
 * Upstream sizes it so `struct dentry` lands on a cache line; the value is a
 * trade-off, not a limit — a longer name still works, it just allocates.
 */
#define DNAME_INLINE_LEN 32

struct dentry {
	unsigned int d_flags;
	seqcount_t d_seq;
	struct hlist_bl_node d_hash;
	struct dentry *d_parent;
	struct qstr d_name;
	struct inode *d_inode;    /* NULL for a negative dentry — a name known
	                           * not to exist, which is a real answer */
	unsigned char d_iname[DNAME_INLINE_LEN];
	struct lockref d_lockref;
	const struct dentry_operations *d_op;
	struct super_block *d_sb;
	unsigned long d_time;
	void *d_fsdata;
	struct list_head d_child;
	struct list_head d_subdirs;
	struct hlist_node d_u;
};

#define DCACHE_OP_HASH        0x00000001
#define DCACHE_OP_COMPARE     0x00000002
#define DCACHE_OP_REVALIDATE  0x00000004
#define DCACHE_OP_DELETE      0x00000008
#define DCACHE_DISCONNECTED   0x00000004
#define DCACHE_ENTRY_TYPE     0x00700000
/* This name was renamed away by an NFS client and is being kept only until the
 * last reference goes. btrfs tests it when deciding whether a subvolume's
 * dentry may be deleted. */
#define DCACHE_NFSFS_RENAMED  0x00001000

static inline struct inode *d_inode(const struct dentry *dentry)
{
	return dentry->d_inode;
}

static inline struct inode *d_really_is_positive(const struct dentry *dentry)
{
	return dentry->d_inode;
}

static inline bool d_is_dir(const struct dentry *dentry)
{
	return dentry->d_inode && S_ISDIR(dentry->d_inode->i_mode);
}

static inline bool d_is_negative(const struct dentry *dentry)
{
	return dentry->d_inode == NULL;
}

static inline bool d_unhashed(const struct dentry *dentry)
{
	return hlist_bl_unhashed(&dentry->d_hash);
}

/*
 * A dentry with no parent but itself. That is what a mount root is, and it is
 * how a path walk knows to stop rather than stepping off the top.
 */
#define IS_ROOT(x) ((x) == (x)->d_parent)

static inline bool d_really_is_negative(const struct dentry *dentry)
{ return dentry->d_inode == NULL; }
static inline bool d_is_positive(const struct dentry *dentry)
{ return dentry->d_inode != NULL; }

struct dentry *dget_parent(struct dentry *dentry);
/* Any name this inode is known by. An inode can have several (hard links) and
 * this returns whichever is to hand — callers use it to name the file in a
 * message or to find a parent to fsync, never as THE name. */
struct dentry *d_find_any_alias(struct inode *inode);

struct dentry *d_make_root(struct inode *root_inode);
struct dentry *d_obtain_root(struct inode *inode);
struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry);
struct dentry *d_find_alias(struct inode *inode);
void d_instantiate(struct dentry *dentry, struct inode *inode);
void d_instantiate_new(struct dentry *dentry, struct inode *inode);
void d_add(struct dentry *dentry, struct inode *inode);
void d_delete(struct dentry *dentry);
void d_drop(struct dentry *dentry);
void d_invalidate(struct dentry *dentry);
void d_move(struct dentry *dentry, struct dentry *target);
void d_tmpfile(struct file *file, struct inode *inode);
void dput(struct dentry *dentry);
struct dentry *dget(struct dentry *dentry);
void shrink_dcache_sb(struct super_block *sb);
void d_prune_aliases(struct inode *inode);
char *dentry_path_raw(const struct dentry *dentry, char *buf, int buflen);
void d_set_d_op(struct dentry *dentry, const struct dentry_operations *op);
int d_set_mounted(struct dentry *dentry);

/* ── file ───────────────────────────────────────────────────────── */

struct path;

/*
 * The read-ahead window a file carries, so sequential reads grow their
 * requests. `ra_pages` comes from the backing device; a filesystem may raise it
 * at mount, and btrfs does.
 */
struct file_ra_state {
	pgoff_t start;
	unsigned int size;
	unsigned int async_size;
	unsigned int ra_pages;
	unsigned int mmap_miss;
	loff_t prev_pos;
};

struct file {
	void *private_data;
	unsigned int f_flags;
	loff_t f_pos;
	struct inode *f_inode;
	const struct file_operations *f_op;
	struct address_space *f_mapping;
	fmode_t f_mode;
	atomic_long_t f_count;
	/* The b1nix handle this file is installed behind, once a descriptor has
	 * been assigned. NULL before fd_install. */
	void *f_handle;

	/* The (mount, dentry) this file was opened through. A filesystem needs it
	 * to name the file — btrfs's ioctls resolve subvolumes from it. */
	struct path f_path;
	struct file_ra_state f_ra;
	u64 f_version;
	/* The writeback error this file has already reported. The point of the
	 * sequence is that each open file reports a given error once: a second
	 * fsync on the same fd returns success, a fresh open sees the error. */
	errseq_t f_wb_err;
	errseq_t f_sb_err;
};

#define FMODE_READ            0x1
#define FMODE_WRITE           0x2
#define FMODE_LSEEK           0x4
#define FMODE_PREAD           0x8
#define FMODE_PWRITE          0x10
#define FMODE_EXEC            0x20
#define FMODE_NDELAY          0x40
#define FMODE_EXCL            0x80
#define FMODE_WRITE_IOCTL     0x100
#define FMODE_32BITHASH       0x200
#define FMODE_64BITHASH       0x400
#define FMODE_NOCMTIME        0x800
#define FMODE_RANDOM          0x1000
#define FMODE_UNSIGNED_OFFSET 0x2000
#define FMODE_NONOTIFY        0x4000000
#define FMODE_CAN_ODIRECT     0x400000
#define FMODE_BUF_RASYNC      0x40000000
#define FMODE_BUF_WASYNC      0x80000000

#define no_llseek NULL
#define noop_llseek NULL

static inline struct inode *file_inode(const struct file *f)
{
	return f ? f->f_inode : NULL;
}

static inline struct dentry *file_dentry(const struct file *file)
{
	return file->f_path.dentry;
}

struct kiocb {
	struct file *ki_filp;
	loff_t ki_pos;
	void (*ki_complete)(struct kiocb *iocb, long ret);
	void *private;
	int ki_flags;
	u16 ki_ioprio;
	/*
	 * Called to finish a direct I/O in the SUBMITTING task's context rather
	 * than in the completion's. iomap uses it when the completion needs to do
	 * work that cannot run from an interrupt — updating i_size, say — and the
	 * distinction from ki_complete is which context runs it.
	 */
	ssize_t (*dio_complete)(void *data);
};

#define IOCB_EVENTFD    (1 << 0)
#define IOCB_APPEND     (1 << 1)
#define IOCB_DIRECT     (1 << 2)
#define IOCB_HIPRI      (1 << 3)
#define IOCB_DSYNC      (1 << 4)
#define IOCB_SYNC       (1 << 5)
#define IOCB_WRITE      (1 << 6)
#define IOCB_NOWAIT     (1 << 7)
#define IOCB_WAITQ      (1 << 8)
#define IOCB_NOIO       (1 << 9)
#define IOCB_ALLOC_CACHE (1 << 10)
#define IOCB_DIO_CALLER_COMP (1 << 11)

static inline bool is_sync_kiocb(struct kiocb *kiocb)
{
	return kiocb->ki_complete == NULL;
}

/* ── operations vectors ─────────────────────────────────────────── */

struct file_operations {
	struct module *owner;
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);
	ssize_t (*write_iter)(struct kiocb *, struct iov_iter *);
	int (*iopoll)(struct kiocb *kiocb, struct io_comp_batch *, unsigned int);
	int (*iterate_shared)(struct file *, struct dir_context *);
	__poll_t (*poll)(struct file *, poll_table *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int (*mmap)(struct file *, struct vm_area_struct *);
	unsigned long mmap_supported_flags;
	int (*open)(struct inode *, struct file *);
	int (*flush)(struct file *, fl_owner_t id);
	int (*release)(struct inode *, struct file *);
	int (*fsync)(struct file *, loff_t, loff_t, int datasync);
	int (*fasync)(int, struct file *, int);
	int (*lock)(struct file *, int, struct file_lock *);
	unsigned long (*get_unmapped_area)(struct file *, unsigned long,
	                                   unsigned long, unsigned long,
	                                   unsigned long);
	int (*check_flags)(int);
	int (*flock)(struct file *, int, struct file_lock *);
	ssize_t (*splice_write)(struct pipe_inode_info *, struct file *, loff_t *,
	                        size_t, unsigned int);
	ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *,
	                       size_t, unsigned int);
	void (*splice_eof)(struct file *file);
	int (*setlease)(struct file *, int, struct file_lock **, void **);
	long (*fallocate)(struct file *file, int mode, loff_t offset, loff_t len);
	void (*show_fdinfo)(struct seq_file *m, struct file *f);
	ssize_t (*copy_file_range)(struct file *, loff_t, struct file *, loff_t,
	                           size_t, unsigned int);
	loff_t (*remap_file_range)(struct file *file_in, loff_t pos_in,
	                           struct file *file_out, loff_t pos_out,
	                           loff_t len, unsigned int remap_flags);
	int (*fadvise)(struct file *, loff_t, loff_t, int);
	int (*uring_cmd)(struct io_uring_cmd *ioucmd, unsigned int issue_flags);
};

struct inode_operations {
	struct dentry *(*lookup)(struct inode *, struct dentry *, unsigned int);
	const char *(*get_link)(struct dentry *, struct inode *,
	                        struct delayed_call *);
	int (*permission)(struct mnt_idmap *, struct inode *, int);
	struct posix_acl *(*get_inode_acl)(struct inode *, int, bool);
	int (*readlink)(struct dentry *, char __user *, int);
	int (*create)(struct mnt_idmap *, struct inode *, struct dentry *,
	              umode_t, bool);
	int (*link)(struct dentry *, struct inode *, struct dentry *);
	int (*unlink)(struct inode *, struct dentry *);
	int (*symlink)(struct mnt_idmap *, struct inode *, struct dentry *,
	               const char *);
	int (*mkdir)(struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
	int (*rmdir)(struct inode *, struct dentry *);
	int (*mknod)(struct mnt_idmap *, struct inode *, struct dentry *,
	             umode_t, dev_t);
	int (*rename)(struct mnt_idmap *, struct inode *, struct dentry *,
	              struct inode *, struct dentry *, unsigned int);
	int (*setattr)(struct mnt_idmap *, struct dentry *, struct iattr *);
	int (*getattr)(struct mnt_idmap *, const struct path *, struct kstat *,
	               u32, unsigned int);
	ssize_t (*listxattr)(struct dentry *, char *, size_t);
	int (*fiemap)(struct inode *, struct fiemap_extent_info *, u64 start,
	              u64 len);
	int (*update_time)(struct inode *, int);
	int (*atomic_open)(struct inode *, struct dentry *, struct file *,
	                   unsigned open_flag, umode_t create_mode);
	int (*tmpfile)(struct mnt_idmap *, struct inode *, struct file *, umode_t);
	struct posix_acl *(*get_acl)(struct mnt_idmap *, struct dentry *, int);
	int (*set_acl)(struct mnt_idmap *, struct dentry *, struct posix_acl *,
	               int);
	int (*fileattr_set)(struct mnt_idmap *idmap, struct dentry *dentry,
	                    struct fileattr *fa);
	int (*fileattr_get)(struct dentry *dentry, struct fileattr *fa);
};

enum freeze_holder {
	FREEZE_HOLDER_KERNEL = (1U << 0),
	FREEZE_HOLDER_USERSPACE = (1U << 1),
};

struct super_operations {
	struct inode *(*alloc_inode)(struct super_block *sb);
	void (*destroy_inode)(struct inode *);
	void (*free_inode)(struct inode *);
	void (*dirty_inode)(struct inode *, int flags);
	int (*write_inode)(struct inode *, struct writeback_control *wbc);
	int (*drop_inode)(struct inode *);
	void (*evict_inode)(struct inode *);
	void (*put_super)(struct super_block *);
	int (*sync_fs)(struct super_block *sb, int wait);
	int (*freeze_super)(struct super_block *, enum freeze_holder who);
	int (*freeze_fs)(struct super_block *);
	int (*thaw_super)(struct super_block *, enum freeze_holder who);
	int (*unfreeze_fs)(struct super_block *);
	int (*statfs)(struct dentry *, struct kstatfs *);
	int (*remount_fs)(struct super_block *, int *, char *);
	void (*umount_begin)(struct super_block *);
	int (*show_options)(struct seq_file *, struct dentry *);
	int (*show_devname)(struct seq_file *, struct dentry *);
	int (*show_path)(struct seq_file *, struct dentry *);
	int (*show_stats)(struct seq_file *, struct dentry *);
	ssize_t (*quota_read)(struct super_block *, int, char *, size_t, loff_t);
	ssize_t (*quota_write)(struct super_block *, int, const char *, size_t,
	                       loff_t);
	struct dquot **(*get_dquots)(struct inode *);
	long (*nr_cached_objects)(struct super_block *,
	                          struct shrink_control *);
	long (*free_cached_objects)(struct super_block *,
	                            struct shrink_control *);
	void (*shutdown)(struct super_block *sb);
};

/* ── super_block ────────────────────────────────────────────────── */

/* s_flags */
#define SB_RDONLY      (1 << 0)
#define SB_NOSUID      (1 << 1)
#define SB_NODEV       (1 << 2)
#define SB_NOEXEC      (1 << 3)
#define SB_SYNCHRONOUS (1 << 4)
#define SB_MANDLOCK    (1 << 6)
#define SB_DIRSYNC     (1 << 7)
#define SB_NOATIME     (1 << 10)
#define SB_NODIRATIME  (1 << 11)
#define SB_SILENT      (1 << 15)
#define SB_POSIXACL    (1 << 16)
#define SB_INLINECRYPT (1 << 17)
#define SB_KERNMOUNT   (1 << 22)
#define SB_I_VERSION   (1 << 23)
#define SB_LAZYTIME    (1 << 25)
/* The superblock has finished being filled in. Before this, `s_fs_info` and the
 * root are still being built and nothing outside the mount may look at them —
 * which is what a filesystem's error path tests before trying to report through
 * a superblock that may not be there yet. */
/* No setuid/setgid bit to strip on write: the filesystem has already checked
 * and there is nothing to remove, so the VFS may skip the check per write. It
 * is a cache of a fact, and setting it wrongly leaves a setuid bit on a file
 * that has been written. */
#define SB_NOSEC       (1 << 28)
#define SB_BORN        (1 << 29)
#define SB_ACTIVE      (1 << 30)
#define SB_NOUSER      (1 << 31)

/* s_iflags */
#define SB_I_CGROUPWB      0x00000001
#define SB_I_NOEXEC        0x00000002
#define SB_I_NODEV         0x00000004
#define SB_I_STABLE_WRITES 0x00000008
#define SB_I_USERNS_VISIBLE 0x00000010
#define SB_I_IMA_UNVERIFIABLE_SIGNATURE 0x00000020
#define SB_I_UNTRUSTED_MOUNTER 0x00000040
#define SB_I_EVM_UNSUPPORTED 0x00000080
#define SB_I_SKIP_SYNC     0x00000100
#define SB_I_PERSB_BDI     0x00000200
#define SB_I_TS_EXPIRY_WARNED 0x00000400
#define SB_I_RETIRED       0x00000800

enum sb_writers_level {
	SB_FREEZE_WRITE = 1,
	SB_FREEZE_PAGEFAULT = 2,
	SB_FREEZE_FS = 3,
	SB_FREEZE_COMPLETE = 4,
};

#define SB_FREEZE_LEVELS (SB_FREEZE_COMPLETE - 1)

/*
 * The freeze levels, and why there are three of them: a freeze has to stop
 * ordinary writes first, then page faults (which are writes arriving by another
 * door), then the filesystem's own internal work. Draining them in one step
 * deadlocks, because the internal work is what completes the writes already in
 * flight.
 */
struct sb_writers {
	unsigned short frozen;
	int freeze_kcount;
	int freeze_ucount;
	struct percpu_rw_semaphore rw_sem[SB_FREEZE_LEVELS];
};

struct super_block {
	struct list_head s_list;
	dev_t s_dev;
	unsigned char s_blocksize_bits;
	unsigned long s_blocksize;
	loff_t s_maxbytes;
	struct file_system_type *s_type;
	const struct super_operations *s_op;
	const struct dquot_operations *dq_op;
	const struct quotactl_ops *s_qcop;
	const struct export_operations *s_export_op;
	unsigned long s_flags;
	unsigned long s_iflags;
	unsigned long s_magic;
	struct dentry *s_root;
	struct rw_semaphore s_umount;
	int s_count;
	atomic_t s_active;
	const struct xattr_handler *const *s_xattr;
	const struct fscrypt_operations *s_cop;
	struct fsverity_operations *s_vop;
	struct unicode_map *s_encoding;
	__u16 s_encoding_flags;
	struct hlist_bl_head s_roots;
	struct list_head s_mounts;
	struct block_device *s_bdev;
	struct backing_dev_info *s_bdi;
	struct list_head s_inodes;
	struct list_head s_inodes_wb;
	struct mtd_info *s_mtd;
	struct hlist_node s_instances;
	unsigned int s_quota_types;
	struct quota_info s_dquot;
	struct sb_writers s_writers;
	void *s_fs_info;
	u32 s_time_gran;              /* timestamp granularity, nanoseconds */
	time64_t s_time_min;
	time64_t s_time_max;
	char s_id[32];
	u8 s_uuid[16];
	u8 s_uuid_len;
	unsigned int s_max_links;
	fmode_t s_mode;
	struct mutex s_vfs_rename_mutex;
	const char *s_subtype;
	const struct dentry_operations *s_d_op;
	int s_readonly_remount;
	errseq_t s_wb_err;
	struct workqueue_struct *s_dio_done_wq;
	spinlock_t s_inode_list_lock;
	spinlock_t s_inode_wblist_lock;
	struct mutex s_sync_lock;
	int s_stack_depth;
	struct user_namespace *s_user_ns;
	/*
	 * The shrinker that reclaims this superblock's caches. Embedded rather
	 * than a pointer because its lifetime is the superblock's, and both
	 * filesystems take its address to rename it once they know the device.
	 */
	struct shrinker s_shrink;
};

static inline bool sb_rdonly(const struct super_block *sb)
{
	return (sb->s_flags & SB_RDONLY) != 0;
}

/*
 * Take a write reference on the superblock, so a freeze waits for it.
 *
 * `sb_start_write` may sleep and must be paired with `sb_end_write` on every
 * path out — including the error paths, which is where an unpaired one hides:
 * a leaked reference does not fail anything until somebody tries to freeze the
 * filesystem, at which point it hangs.
 */
void sb_start_write(struct super_block *sb);
bool sb_start_write_trylock(struct super_block *sb);
void sb_end_write(struct super_block *sb);
void sb_start_pagefault(struct super_block *sb);
void sb_end_pagefault(struct super_block *sb);
void sb_start_intwrite(struct super_block *sb);
bool sb_start_intwrite_trylock(struct super_block *sb);
void sb_end_intwrite(struct super_block *sb);

int sb_set_blocksize(struct super_block *sb, int size);
int sb_min_blocksize(struct super_block *sb, int size);

struct file_system_type {
	const char *name;
	int fs_flags;
	int (*init_fs_context)(struct fs_context *);
	const struct fs_parameter_spec *parameters;
	struct dentry *(*mount)(struct file_system_type *, int, const char *,
	                        void *);
	void (*kill_sb)(struct super_block *);
	struct module *owner;
	struct file_system_type *next;
	struct hlist_head fs_supers;
};

#define FS_REQUIRES_DEV       1
#define FS_BINARY_MOUNTDATA   2
#define FS_HAS_SUBTYPE        4
#define FS_USERNS_MOUNT       8
#define FS_DISALLOW_NOTIFY_PERM 16
#define FS_ALLOW_IDMAP        32
#define FS_RENAME_DOES_D_MOVE 32768

int register_filesystem(struct file_system_type *fs);
int unregister_filesystem(struct file_system_type *fs);
struct file_system_type *get_fs_type(const char *name);

struct super_block *sget(struct file_system_type *type,
                         int (*test)(struct super_block *, void *),
                         int (*set)(struct super_block *, void *), int flags,
                         void *data);
struct super_block *sget_fc(struct fs_context *fc,
                            int (*test)(struct super_block *,
                                        struct fs_context *),
                            int (*set)(struct super_block *,
                                       struct fs_context *));
void deactivate_super(struct super_block *sb);
void deactivate_locked_super(struct super_block *sb);
void kill_block_super(struct super_block *sb);
void kill_anon_super(struct super_block *sb);
void generic_shutdown_super(struct super_block *sb);
struct dentry *mount_bdev(struct file_system_type *fs_type, int flags,
                          const char *dev_name, void *data,
                          int (*fill_super)(struct super_block *, void *, int));

struct vfsmount {
	struct super_block *mnt_sb;
	struct dentry *mnt_root;
	int mnt_flags;
};

int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount,
                  int *count);
void simple_release_fs(struct vfsmount **mount, int *count);
/* Flush a block device's own cache of metadata. Called at unmount and before a
 * superblock write that must be ordered against everything before it. */
int sync_blockdev(struct block_device *bdev);
int sync_blockdev_range(struct block_device *bdev, loff_t lstart, loff_t lend);
void invalidate_bdev(struct block_device *bdev);
/* Resolve a path to the block device it names, without opening it. */
dev_t lookup_bdev(const char *pathname, dev_t *dev);

struct vfsmount *vfs_kern_mount(struct file_system_type *type, int flags,
                                const char *name, void *data);
void kern_unmount(struct vfsmount *mnt);

/* ── idmapped mounts ────────────────────────────────────────────── */

/*
 * b1nix has no idmapped mounts, so every mapping is the identity — but the
 * parameter is threaded through every inode operation and cannot be removed
 * without editing imported code. `nop_mnt_idmap` is the one instance.
 */
extern struct mnt_idmap nop_mnt_idmap;


static inline kuid_t i_uid_into_vfsuid(struct mnt_idmap *idmap,
                                       const struct inode *inode)
{ (void)idmap; return inode->i_uid; }
static inline kgid_t i_gid_into_vfsgid(struct mnt_idmap *idmap,
                                       const struct inode *inode)
{ (void)idmap; return inode->i_gid; }
static inline void inode_fsuid_set(struct inode *inode, struct mnt_idmap *idmap)
{ (void)idmap; inode->i_uid = current_fsuid(); }
static inline void inode_fsgid_set(struct inode *inode, struct mnt_idmap *idmap)
{ (void)idmap; inode->i_gid = current_fsgid(); }

/* ── inode lifecycle ────────────────────────────────────────────── */

struct inode *new_inode(struct super_block *sb);
struct inode *iget_locked(struct super_block *sb, unsigned long ino);
struct inode *ilookup(struct super_block *sb, unsigned long ino);
struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
                       int (*test)(struct inode *, void *), void *data);
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
                           int (*test)(struct inode *, void *),
                           int (*set)(struct inode *, void *), void *data);
struct inode *igrab(struct inode *inode);
void iput(struct inode *inode);
/* Another reference on an inode already known to be live — the caller holds a
 * lock that keeps it so, and this is deliberately NOT the full iget: it must
 * not resurrect an inode that is being freed. */
void __iget(struct inode *inode);
void ihold(struct inode *inode);
void iget_failed(struct inode *inode);
void unlock_new_inode(struct inode *inode);
void discard_new_inode(struct inode *inode);
void clear_inode(struct inode *inode);
void inode_init_once(struct inode *inode);
void inode_init_owner(struct mnt_idmap *idmap, struct inode *inode,
                      const struct inode *dir, umode_t mode);
void __insert_inode_hash(struct inode *inode, unsigned long hashval);
void remove_inode_hash(struct inode *inode);
int insert_inode_locked4(struct inode *inode, unsigned long hashval,
                         int (*test)(struct inode *, void *), void *data);
int generic_delete_inode(struct inode *inode);
int inode_needs_sync(struct inode *inode);
void truncate_inode_pages(struct address_space *mapping, loff_t lstart);
void truncate_inode_pages_final(struct address_space *mapping);
void truncate_inode_pages_range(struct address_space *mapping, loff_t lstart,
                                loff_t lend);
void truncate_pagecache(struct inode *inode, loff_t newsize);
void truncate_setsize(struct inode *inode, loff_t newsize);
int inode_newsize_ok(const struct inode *inode, loff_t offset);

static inline void insert_inode_hash(struct inode *inode)
{
	__insert_inode_hash(inode, inode->i_ino);
}

void __mark_inode_dirty(struct inode *inode, int flags);

static inline void mark_inode_dirty(struct inode *inode)
{
	__mark_inode_dirty(inode, I_DIRTY);
}

static inline void mark_inode_dirty_sync(struct inode *inode)
{
	__mark_inode_dirty(inode, I_DIRTY_SYNC);
}

/*
 * "May the caller act as this file's owner?"
 *
 * True for the owner, and for a caller with CAP_FOWNER. It is the check behind
 * chattr, chmod and the ioctls that change a file's layout — a version that
 * only compared uids would refuse root, and one that only checked the
 * capability would let any process change any file.
 */
bool inode_owner_or_capable(struct mnt_idmap *idmap, const struct inode *inode);

int inode_permission(struct mnt_idmap *idmap, struct inode *inode, int mask);
int generic_permission(struct mnt_idmap *idmap, struct inode *inode, int mask);
int setattr_prepare(struct mnt_idmap *idmap, struct dentry *dentry,
                    struct iattr *attr);
void setattr_copy(struct mnt_idmap *idmap, struct inode *inode,
                  const struct iattr *attr);
int inode_setattr(struct inode *inode, struct iattr *attr);
void generic_fillattr(struct mnt_idmap *idmap, u32 request_mask,
                      struct inode *inode, struct kstat *stat);
int file_remove_privs(struct file *file);
int file_update_time(struct file *file);
int inode_update_time(struct inode *inode, int flags);
int generic_update_time(struct inode *inode, int flags);
void touch_atime(const struct path *path);

/*
 * Which timestamps an update_time call is refreshing. They are flags rather
 * than one "touch everything" because a read updates atime alone and a write
 * updates mtime and ctime — refreshing all three on a read would dirty the
 * inode on every access.
 */
#define S_ATIME   1
#define S_MTIME   2
#define S_CTIME   4
#define S_VERSION 8

/* An inode currently open for writing by somebody. */
static inline bool inode_is_open_for_write(const struct inode *inode)
{ return atomic_read(&((struct inode *)inode)->i_writecount) > 0; }
void inode_io_list_del(struct inode *inode);

/* Set the block size a block device's own page cache uses. Fails if the size
 * is not a power of two within the device's limits — which is a real answer: a
 * filesystem asking for a block size the device cannot address must not mount. */
int set_blocksize(struct block_device *bdev, int size);

#define MAY_EXEC   0x00000001
#define MAY_WRITE  0x00000002
#define MAY_READ   0x00000004
#define MAY_APPEND 0x00000008
#define MAY_ACCESS 0x00000010
#define MAY_OPEN   0x00000020
#define MAY_CHDIR  0x00000040
#define MAY_NOT_BLOCK 0x00000080

/* ── generic file operations ────────────────────────────────────── */

/*
 * Reading a directory as if it were a file. Returns -EISDIR: a directory's
 * bytes are not readable, and this is the operations-vector entry that says so
 * rather than leaving `read` NULL, which would give EINVAL.
 */
ssize_t generic_read_dir(struct file *filp, char __user *buf, size_t siz,
                         loff_t *ppos);

/* The compat ioctl entry for a filesystem whose ioctls take no pointers that
 * need converting: it re-enters the native handler. Named rather than left
 * NULL, because NULL means "no compat ioctls at all". */
long compat_ptr_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/*
 * Drop and retake the directory lock, so a long readdir does not hold it
 * across the whole directory. The caller must re-validate everything it cached
 * across the gap — which is why it returns whether the directory is still
 * there.
 */
int dir_relax(struct inode *inode);
int dir_relax_shared(struct inode *inode);

/* Is this index inside the window the read-ahead state currently describes? */
static inline bool ra_has_index(struct file_ra_state *ra, pgoff_t index)
{
	return index >= ra->start && index < ra->start + ra->size;
}

/*
 * "Where does this file offset live on disk?"
 *
 * The callback a filesystem gives the generic block helpers. `create` says
 * whether it may allocate — a read passes 0 and gets a hole reported as an
 * unmapped buffer, a write passes 1. Confusing the two either allocates on
 * every read or writes into a hole that was never allocated.
 */
struct buffer_head;
typedef int (get_block_t)(struct inode *inode, sector_t iblock,
                          struct buffer_head *bh_result, int create);

int generic_file_open(struct inode *inode, struct file *filp);
ssize_t generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter);
ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from);
ssize_t __generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from);
ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *iter);
int generic_write_checks(struct kiocb *iocb, struct iov_iter *from);
int generic_file_mmap(struct file *file, struct vm_area_struct *vma);
int generic_file_readonly_mmap(struct file *file, struct vm_area_struct *vma);
loff_t generic_file_llseek(struct file *file, loff_t offset, int whence);
loff_t generic_file_llseek_size(struct file *file, loff_t offset, int whence,
                                loff_t maxsize, loff_t eof);
loff_t default_llseek(struct file *file, loff_t offset, int whence);
loff_t no_seek_end_llseek(struct file *file, loff_t offset, int whence);
loff_t vfs_setpos(struct file *file, loff_t offset, loff_t maxsize);
ssize_t generic_file_splice_read(struct file *in, loff_t *ppos,
                                 struct pipe_inode_info *pipe, size_t len,
                                 unsigned int flags);
ssize_t iter_file_splice_write(struct pipe_inode_info *pipe, struct file *out,
                               loff_t *ppos, size_t len, unsigned int flags);
int generic_file_fsync(struct file *file, loff_t start, loff_t end,
                       int datasync);
int file_write_and_wait_range(struct file *file, loff_t lstart, loff_t lend);
int file_check_and_advance_wb_err(struct file *file);
int filemap_write_and_wait_range(struct address_space *mapping, loff_t lstart,
                                 loff_t lend);
int filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
                             loff_t end);
int filemap_fdatawait_range(struct address_space *mapping, loff_t start,
                            loff_t end);
int filemap_flush(struct address_space *mapping);
int filemap_fdatawrite(struct address_space *mapping);
int sync_inode_metadata(struct inode *inode, int wait);
int sync_filesystem(struct super_block *sb);
void sync_inodes_sb(struct super_block *sb);
int write_inode_now(struct inode *inode, int sync);

/* ── whence ─────────────────────────────────────────────────────── */

#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#define SEEK_DATA 3
#define SEEK_HOLE 4
#define SEEK_MAX  SEEK_HOLE

/* The largest offset a file can address. 64-bit only here, so it is the signed
 * maximum the page-index arithmetic stays inside. */
#define MAX_LFS_FILESIZE ((loff_t)LLONG_MAX)

/* Rename flags, which are ABI. */
#define RENAME_NOREPLACE (1 << 0)
#define RENAME_EXCHANGE  (1 << 1)
#define RENAME_WHITEOUT  (1 << 2)

/* remap_file_range flags. */
#define REMAP_FILE_DEDUP        (1 << 0)
#define REMAP_FILE_CAN_SHORTEN  (1 << 1)
#define REMAP_FILE_ADVISORY     (REMAP_FILE_CAN_SHORTEN)

loff_t generic_remap_file_range_prep(struct file *file_in, loff_t pos_in,
                                     struct file *file_out, loff_t pos_out,
                                     loff_t *len, unsigned int remap_flags);

/* ── things the DRM core needs from this header ─────────────────── */

struct file *file_clone_open(struct file *f);
struct inode *alloc_anon_inode(struct super_block *sb);
struct file *get_file_rcu(struct file *f);

static inline int register_chrdev(unsigned int major, const char *name,
                                  const struct file_operations *fops)
{ (void)major; (void)name; (void)fops; return 0; }
static inline void unregister_chrdev(unsigned int major, const char *name)
{ (void)major; (void)name; }

#define replace_fops(f, new_fops) do { (f)->f_op = (new_fops); } while (0)
#define fops_get(fops) (fops)
#define fops_put(fops) do { (void)(fops); } while (0)

/* The major/minor split is the one userspace reads out of stat(2): twenty bits
 * of minor, the rest major. Spelled out here rather than through the MINOR
 * macro so this header does not depend on where <linux/kdev_t.h> falls in the
 * include order. */
static inline unsigned int iminor(const struct inode *inode)
{
	return inode ? (unsigned int)(inode->i_ino & 0xfffffu) : 0;
}
static inline unsigned int imajor(const struct inode *inode)
{
	return inode ? (unsigned int)(inode->i_ino >> 20) : 0;
}

static inline int call_mmap(struct file *file, struct vm_area_struct *vma)
{
	return (file && file->f_op && file->f_op->mmap)
	           ? file->f_op->mmap(file, vma)
	           : -ENODEV;
}


/* ── the rest of the VFS surface a filesystem calls ─────────────── */

/*
 * Directory entry types as they appear ON DISK in ext4 and btrfs, which is a
 * different numbering from the DT_* the getdents ABI uses. Both filesystems
 * convert between them on every readdir, and the two conversion helpers are the
 * only place that mapping should live — an open-coded one that got a pair wrong
 * would report directories as sockets.
 */
#define FT_UNKNOWN  0
#define FT_REG_FILE 1
#define FT_DIR      2
#define FT_CHRDEV   3
#define FT_BLKDEV   4
#define FT_FIFO     5
#define FT_SOCK     6
#define FT_SYMLINK  7
#define FT_MAX      8

unsigned char fs_umode_to_ftype(umode_t mode);
unsigned char fs_umode_to_dtype(umode_t mode);
unsigned char fs_ftype_to_dtype(unsigned int filetype);

/* A whiteout: a name that hides a lower layer's entry in an overlay. It is a
 * character device with device number zero, which is a value no real device
 * can have — that is the whole trick. */
#define WHITEOUT_MODE 0020000
#define WHITEOUT_DEV  0

#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)
#define BLOCK_SIZE_BITS 10
#define BLOCK_SIZE (1 << BLOCK_SIZE_BITS)

/* An inode has fallen off the hash: nothing can look it up any more. */
static inline bool inode_unhashed(struct inode *inode)
{ return hlist_unhashed(&inode->i_hash); }
static inline bool inode_is_dirtytime_only(struct inode *inode)
{ return (inode->i_state & (I_DIRTY_TIME | I_NEW | I_FREEING | I_WILL_FREE)) ==
         I_DIRTY_TIME; }

void inode_inc_link_count(struct inode *inode);
void inode_dec_link_count(struct inode *inode);
void inode_set_flags(struct inode *inode, unsigned int flags,
                     unsigned int mask);
void inode_nohighmem(struct inode *inode);
/* Returns whether anything changed, so the caller knows to mark the inode
 * dirty — a void version would make every timestamp update dirty the inode,
 * including the ones that wrote the same value back. */
bool inode_update_timestamps(struct inode *inode, int flags);
int insert_inode_locked(struct inode *inode);
struct inode *find_inode_by_ino_rcu(struct super_block *sb, unsigned long ino);
struct inode *alloc_inode_sb(struct super_block *sb, struct kmem_cache *cache,
                             gfp_t gfp);
int generic_drop_inode(struct inode *inode);
void init_special_inode(struct inode *inode, umode_t mode, dev_t rdev);
void invalidate_inode_buffers(struct inode *inode);
void truncate_pagecache_range(struct inode *inode, loff_t offset, loff_t end);

/*
 * Whether an id has to be rewritten when the mount's idmapping changes.
 * Always false here — there are no idmapped mounts — but the calls stay,
 * because a filesystem asks them before writing an inode and removing them
 * would mean editing imported code.
 */
static inline bool i_uid_needs_update(struct mnt_idmap *idmap,
                                      const struct iattr *attr,
                                      const struct inode *inode)
{ (void)idmap; (void)inode; return (attr->ia_valid & ATTR_UID) != 0; }
static inline bool i_gid_needs_update(struct mnt_idmap *idmap,
                                      const struct iattr *attr,
                                      const struct inode *inode)
{ (void)idmap; (void)inode; return (attr->ia_valid & ATTR_GID) != 0; }
static inline void i_uid_update(struct mnt_idmap *idmap,
                                const struct iattr *attr, struct inode *inode)
{ (void)idmap; if (attr->ia_valid & ATTR_UID) inode->i_uid = attr->ia_uid; }
static inline void i_gid_update(struct mnt_idmap *idmap,
                                const struct iattr *attr, struct inode *inode)
{ (void)idmap; if (attr->ia_valid & ATTR_GID) inode->i_gid = attr->ia_gid; }

struct mnt_idmap *file_mnt_idmap(struct file *file);
bool fsuidgid_has_mapping(struct super_block *sb, struct mnt_idmap *idmap);
bool in_group_p(kgid_t grp);
umode_t current_umask(void);
struct user_namespace *current_user_ns(void);

/* Dentries the VFS creates and names. */
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name);
struct dentry *d_obtain_alias(struct inode *inode);
void d_mark_dontcache(struct inode *inode);
void d_delete_notify(struct inode *dir, struct dentry *dentry);
char *d_path(const struct path *path, char *buf, int buflen);
char *file_path(struct file *file, char *buf, int buflen);
void nd_terminate_link(void *name, size_t len, size_t maxlen);
const char *page_get_link(struct dentry *dentry, struct inode *inode,
                          struct delayed_call *done);
const char *simple_get_link(struct dentry *dentry, struct inode *inode,
                            struct delayed_call *done);
void kfree_link(void *p);
void simple_rename_timestamp(struct inode *old_dir, struct dentry *old_dentry,
                             struct inode *new_dir, struct dentry *new_dentry);
int check_sticky(struct mnt_idmap *idmap, struct inode *dir,
                 struct inode *inode);
void lock_two_nondirectories(struct inode *inode1, struct inode *inode2);
void unlock_two_nondirectories(struct inode *inode1, struct inode *inode2);
extern const struct inode_operations simple_dir_inode_operations;
extern const struct file_operations simple_dir_operations;
extern const struct qstr dotdot_name;
void generic_set_encrypted_ci_d_ops(struct dentry *dentry);
int finish_open_simple(struct file *file, int error);

/*
 * Anonymous device numbers, for a filesystem with no block device of its own.
 * They come from a shared pool, so the free is not optional: leaking one leaks
 * a minor number for the life of the boot.
 */
int get_anon_bdev(dev_t *dev);
void free_anon_bdev(dev_t dev);
int set_anon_super(struct super_block *sb, void *data);
int super_setup_bdi(struct super_block *sb);
int super_setup_bdi_name(struct super_block *sb, char *fmt, ...);
struct dentry *mount_subtree(struct vfsmount *mnt, const char *path);
void mntput(struct vfsmount *mnt);
struct mnt_idmap *mnt_idmap(const struct vfsmount *mnt);

/* Can this device be addressed with a block size of `blocksize_bits`? A
 * filesystem asks before mounting: a device larger than its own block
 * addressing can reach would be silently truncated. */
int generic_check_addressable(unsigned stblocksize, u64 num_blocks);
/* Returns the fsid rather than writing through a pointer: callers assign it
 * straight into `buf->f_fsid`. */
__kernel_fsid_t uuid_to_fsid(const __u8 *uuid);
u64 sb_bdev_nr_blocks(struct super_block *sb);
void fsnotify_sb_error(struct super_block *sb, struct inode *inode, int error);

/* Write-side bracketing on a file, which is what a freeze waits for. */
void file_start_write(struct file *file);
void file_end_write(struct file *file);
bool file_start_write_trylock(struct file *file);
void file_accessed(struct file *file);
/* Is a write reference held on this superblock? btrfs asserts it on paths that
 * must only run inside one. The level is implied — the write level — because
 * that is the only one a filesystem asserts about. */
bool sb_write_started(struct super_block *sb);
/*
 * How a superblock opens its block device.
 *
 * Spelled with the numeric flags rather than the BLK_OPEN_* names because
 * <linux/blkdev.h> includes THIS header — reaching the other way would be a
 * cycle. The values are asserted against the names in kernel/lkpi/vfs.c, so a
 * change to either side is a build error rather than a device opened read-only
 * when it should be writable.
 */
static inline unsigned int sb_open_mode(unsigned int sb_flags)
{
	return (1u << 0) | ((sb_flags & SB_RDONLY) ? 0 : (1u << 1));
}
struct blk_holder_ops;
extern const struct blk_holder_ops fs_holder_ops;
extern struct kobject *fs_kobj;

/* kiocb setup and the write-side helpers built on it. */
void init_sync_kiocb(struct kiocb *kiocb, struct file *filp);
int kiocb_set_rw_flags(struct kiocb *ki, unsigned int flags);
static inline bool iocb_is_dsync(const struct kiocb *iocb)
{
	return (iocb->ki_flags & IOCB_DSYNC) ||
	       IS_SYNC(iocb->ki_filp->f_mapping->host);
}
int kiocb_write_and_wait(struct kiocb *iocb, size_t count);
int kiocb_invalidate_pages(struct kiocb *iocb, size_t count);
void kiocb_invalidate_post_direct_write(struct kiocb *iocb, size_t count);
ssize_t generic_write_checks_count(struct kiocb *iocb, loff_t *count);
ssize_t generic_write_sync(struct kiocb *iocb, ssize_t count);
int rw_verify_area(int read_write, struct file *file, const loff_t *ppos,
                   size_t count);
ssize_t kernel_write(struct file *file, const void *buf, size_t count,
                     loff_t *pos);
int bmap(struct inode *inode, sector_t *block);

/* Device number encodings that appear ON DISK: ext4 stores the old 16-bit form
 * for small numbers and the new 32-bit one otherwise, and which one it used is
 * inferred from the value. Getting the pair wrong makes every device node in
 * the filesystem name a different device. */
static inline bool old_valid_dev(dev_t dev)
{ return ((dev >> 20) & 0xfff) < 256 && (dev & 0xfffff) < 256; }
/* old_encode_dev lives in <linux/kdev_t.h> with MAJOR/MINOR, which is where the
 * encoding belongs; only its inverse and the 32-bit pair are added here. */
static inline dev_t old_decode_dev(u16 val)
{ return (dev_t)((((val >> 8) & 255) << 20) | (val & 255)); }
static inline u32 new_encode_dev(dev_t dev)
{
	unsigned major = (dev >> 20) & 0xfff;
	unsigned minor = dev & 0xfffff;

	return (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
}
static inline dev_t new_decode_dev(u32 dev)
{
	unsigned major = (dev & 0xfff00) >> 8;
	unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);

	return (dev_t)((major << 20) | minor);
}

/* The ioctl numbers a filesystem answers. ABI: `lsattr`, `chattr`, `fstrim`
 * and `e2label` send exactly these. */
#define FS_IOC_GETFLAGS    _IOR('f', 1, long)
#define FS_IOC_SETFLAGS    _IOW('f', 2, long)
#define FS_IOC_GETVERSION  _IOR('v', 1, long)
#define FS_IOC_SETVERSION  _IOW('v', 2, long)
#define FS_IOC_FIEMAP      _IOWR('f', 11, struct fiemap)
#define FS_IOC32_GETFLAGS  _IOR('f', 1, int)
#define FS_IOC32_SETFLAGS  _IOW('f', 2, int)
#define FS_IOC32_GETVERSION _IOR('v', 1, int)
#define FS_IOC32_SETVERSION _IOW('v', 2, int)
#define FS_IOC_FSGETXATTR  _IOR('X', 31, struct fsxattr)
#define FS_IOC_FSSETXATTR  _IOW('X', 32, struct fsxattr)
#define FS_IOC_GETFSLABEL  _IOR(0x94, 49, char[FSLABEL_MAX])
#define FS_IOC_SETFSLABEL  _IOW(0x94, 50, char[FSLABEL_MAX])
#define FSLABEL_MAX 256
#define FITRIM             _IOWR('X', 121, struct fstrim_range)
#define FICLONE            _IOW(0x94, 9, int)
#define FICLONERANGE       _IOW(0x94, 13, struct file_clone_range)
#define FIDEDUPERANGE      _IOWR(0x94, 54, struct file_dedupe_range)

/* Open flags a filesystem tests. O_SYNC is two bits: __O_SYNC plus O_DSYNC,
 * so that a kernel testing for O_DSYNC catches both — which is why O_SYNC
 * cannot simply be a bit of its own. */
#ifndef O_DSYNC
#define O_DSYNC   00010000
#endif
#ifndef __O_SYNC
#define __O_SYNC  04000000
#endif
#ifndef O_SYNC
#define O_SYNC    (__O_SYNC | O_DSYNC)
#endif

#define FMODE_NOWAIT           0x8000000
#define FMODE_DIO_PARALLEL_WRITE 0x1000000
#define EIOCBQUEUED 529
#define AOP_WRITEPAGE_ACTIVATE 0x80000

/* The largest a file may be for a descriptor that did not ask for large-file
 * support: 2 GiB minus one, which is what a 32-bit off_t can express. */
#define MAX_NON_LFS ((1UL << 31) - 1)
/* The most one read or write may transfer. Larger requests are clamped rather
 * than refused, and the caller loops. */
#define MAX_RW_COUNT (INT_MAX & PAGE_MASK)

#ifndef O_LARGEFILE
#define O_LARGEFILE 0100000
#endif

/* Flush a file's data and metadata over a range. `datasync` skips the metadata
 * that does not affect reading the data back — a size change is not skippable,
 * an atime is. */
int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync);
int vfs_fsync(struct file *file, int datasync);

/* The identity map used everywhere: there are no idmapped mounts here. */
struct mnt_idmap { int dummy; };

typedef unsigned long ino_t;

#include <linux/file.h>


#endif
