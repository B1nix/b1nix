/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FS_H
#define LKPI_LINUX_FS_H
#include <linux/kdev_t.h>
#include <linux/types.h>
#include <linux/wait.h>
/* Enough of the VFS surface for the core's file_operations tables to compile.
 * b1nix's own VFS structures are different in shape; the bridge is written when
 * a driver's character device is actually registered. */
/* Declared first: a struct first seen inside a prototype's parameter list is a
 * type in that scope, and its pointer then refuses to match the file-scope one. */
struct super_block;
struct inode;
struct dentry;
struct file;
struct seq_file;
struct file_operations;
struct address_space;
struct module;
struct vm_area_struct;

/* Enough of a file for the core's private_data and f_flags accesses. b1nix's
 * own file objects are a different shape; the bridge is written when a driver's
 * character device is actually registered. */
struct file {
	void *private_data;
	unsigned int f_flags;
	loff_t f_pos;
	struct inode *f_inode;
	const struct file_operations *f_op;
	/* The page cache this file's pages live in. b1nix's cache is keyed
	 * differently (fs_id, ino); the bridge lands with the mmap path. */
	struct address_space *f_mapping;
	/* Read/write permission the descriptor was opened with, as the VFS
	 * records it — distinct from f_flags, which carries the open mode. */
	fmode_t f_mode;
	/* References to this file. The last put runs the driver's release. */
	atomic_long_t f_count;
	/* The b1nix handle this file is installed behind, once a descriptor has
	 * been assigned. NULL before fd_install. */
	void *f_handle;

};
struct poll_table_struct;
typedef struct poll_table_struct poll_table;
struct file_operations {
	struct module *owner;
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int (*mmap)(struct file *, struct vm_area_struct *);
	int (*open)(struct inode *, struct file *);
	int (*release)(struct inode *, struct file *);
	unsigned int (*poll)(struct file *, poll_table *);
	int (*fasync)(int, struct file *, int);
};
/* An inode, as far as the core names it: the DRM device's minor lives on one,
 * and drm_prime compares them to recognise its own buffers. */
struct inode {
	unsigned long i_ino;
	umode_t i_mode;
	void *i_private;
	struct address_space *i_mapping;
	loff_t i_size;
};

/* Duplicate an open file with new flags. Needs the VFS bridge; declared so the
 * lease paths compile. */
struct file *file_clone_open(struct file *f);
/* An inode with no name in any directory. Wired up with anon_inodes. */
struct inode *alloc_anon_inode(struct super_block *sb);

/* Tearing down a nameless superblock. Paired with the pseudo-filesystem an
 * anonymous inode comes from; both land together. */
/* A filesystem type, for the nameless internal filesystem anonymous inodes
 * come from. Nothing registers one yet; the type exists so the core's
 * definitions compile. */
struct fs_context;
struct file_system_type {
	const char *name;
	int fs_flags;
	int (*init_fs_context)(struct fs_context *);
	void (*kill_sb)(struct super_block *);
	struct module *owner;
};

void kill_anon_super(struct super_block *sb);

/* Pin the internal filesystem an anonymous inode is allocated from. Paired
 * with anon_inodes; both land together. */
/* A mounted filesystem instance. Only the internal one anonymous inodes come
 * from is ever created here, and nothing has yet. */
struct vfsmount { struct super_block *mnt_sb; };

int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount,
                  int *count);
void simple_release_fs(struct vfsmount **mount, int *count);

/* The minor number of the device this inode names. */
/* The inode behind an open file. */
/* Drop a reference to an inode. The VFS bridge owns the lifetime; declared so
 * the teardown paths compile. */
void iput(struct inode *inode);

/* Take a reference on a file_operations table's module. Nothing is a module
 * here, so the table is returned unchanged. */
/* Swap the file's operations table, as a device does when its open() decides
 * which personality the descriptor has. */
/* Character-device major registration. b1nix registers device nodes through
 * its own VFS; nothing claims a major here, so these succeed and record
 * nothing. */
static inline int register_chrdev(unsigned int major, const char *name,
                                  const struct file_operations *fops)
{ (void)major; (void)name; (void)fops; return 0; }
static inline void unregister_chrdev(unsigned int major, const char *name)
{ (void)major; (void)name; }

#define replace_fops(f, new_fops) do { (f)->f_op = (new_fops); } while (0)

#define fops_get(fops) (fops)
#define fops_put(fops) do { (void)(fops); } while (0)

static inline struct inode *file_inode(const struct file *f)
{ return f ? f->f_inode : 0; }

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

/* Descriptor permission bits, as the VFS records them. UNSIGNED_OFFSET marks a
 * file whose offsets are not to be treated as signed — a DRM device, where the
 * offset is a GEM handle rather than a position. */
#define FMODE_READ            0x1
#define FMODE_WRITE           0x2
#define FMODE_LSEEK           0x4
#define FMODE_UNSIGNED_OFFSET 0x2000

#define no_llseek NULL
#define noop_llseek NULL

/* Descriptor and file reference helpers travel with the VFS interface; drivers
 * call fput without including <linux/file.h> themselves. */
#include <linux/file.h>


/* Calling a file's mmap through its operations table. */
static inline int call_mmap(struct file *file, struct vm_area_struct *vma)
{ return (file && file->f_op && file->f_op->mmap) ? file->f_op->mmap(file, vma) : -ENODEV; }


#define SB_KERNMOUNT (1 << 22)

/*
 * Mounting a filesystem from kernel context, for a driver that wants a private
 * tmpfs instance to back its objects.
 *
 * Declared and deliberately not defined. b1nix mounts through its own VFS by
 * path and has no kernel-internal mount that produces a vfsmount a driver can
 * hold. i915's gemfs is the only caller and falls back to anonymous pages when
 * this path is unavailable — but that fallback is chosen at link time here, not
 * at run time, so the object is left out of the build instead.
 */
struct file_system_type;
struct vfsmount;
struct file_system_type *get_fs_type(const char *name);
struct vfsmount *vfs_kern_mount(struct file_system_type *type, int flags,
                                const char *name, void *data);
void kern_unmount(struct vfsmount *mnt);

/*
 * Take a reference to a file read from an RCU-protected pointer.
 *
 * Declared and deliberately not defined. The whole point upstream is that the
 * pointer may be freed concurrently and the refcount bump must fail rather than
 * resurrect it. b1nix's file table is not RCU-protected — a struct file is freed
 * as soon as its last descriptor closes — so there is no safe way to implement
 * this, and a stub that just bumped the count would be a use-after-free wearing
 * the name of the thing that prevents one.
 */
struct file *get_file_rcu(struct file *f);


/* The page-cache side of an inode. b1nix's page cache is not reachable from
 * here — see <linux/pagemap.h> — so this exists for the fields imported code
 * reads off it, not as a live mapping. */
struct writeback_control;
/* What a page cache dispatches through. Nothing here installs one — see the
 * note on struct address_space — so a driver that calls through it must first
 * have obtained a mapping, which it cannot. */
struct folio;
struct address_space_operations {
	int (*writepage)(struct page *page, struct writeback_control *wbc);
	int (*write_begin)(struct file *, struct address_space *mapping,
	                   loff_t pos, unsigned len, struct page **pagep,
	                   void **fsdata);
	int (*write_end)(struct file *, struct address_space *mapping,
	                 loff_t pos, unsigned len, unsigned copied,
	                 struct page *page, void *fsdata);
};

/* The largest offset a file can address. 64-bit only here, so it is the signed
 * maximum the page-index arithmetic stays inside. */
#define MAX_LFS_FILESIZE ((loff_t)LLONG_MAX)
struct address_space {
	struct inode *host;
	const struct address_space_operations *a_ops;
	unsigned long nrpages;
	gfp_t gfp_mask;
};

#endif
