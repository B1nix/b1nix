/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FSVERITY_H
#define LKPI_LINUX_FSVERITY_H

#include <linux/types.h>
#include <linux/errno.h>

/*
 * fs-verity: read-only files with a Merkle tree, so every block is verified as
 * it is read. b1nix does not implement it.
 *
 * Upstream's !CONFIG_FS_VERITY stubs, with their return values kept exactly:
 * `fsverity_active` says no, so no read path ever calls the verification
 * functions; the ioctls refuse with -EOPNOTSUPP; and the verify functions,
 * which are unreachable while `fsverity_active` is false, report failure rather
 * than success — a stub that said "verified" would be a lie the one time
 * something did reach it.
 */

struct inode;
struct file;
struct bio;
struct folio;
struct page;
struct iattr;
struct work_struct;

/*
 * What a filesystem must provide for fs-verity, even though nothing here calls
 * it: ext4 defines its operations vector unconditionally, and a designated
 * initialiser naming a field the structure does not have is a compile error.
 *
 * The shape is upstream's. `read_merkle_tree_page` is the one whose contract is
 * worth stating: it returns a page of the Merkle tree, which for ext4 lives
 * past the end of the file's data — beyond i_size — which is why it goes
 * through the mapping directly rather than through a read.
 */
struct fsverity_operations {
	int (*begin_enable_verity)(struct file *filp);
	int (*end_enable_verity)(struct file *filp, const void *desc,
	                         size_t desc_size, u64 merkle_tree_size);
	int (*get_verity_descriptor)(struct inode *inode, void *buf,
	                             size_t bufsize);
	struct page *(*read_merkle_tree_page)(struct inode *inode,
	                                      pgoff_t index,
	                                      unsigned long num_ra_pages);
	int (*write_merkle_tree_block)(struct inode *inode, const void *buf,
	                               u64 pos, unsigned int size);
};

/* The on-disk descriptor an fs-verity file carries. Declared complete because
 * ext4 takes its size and reads its fields while storing it as an xattr. */
struct fsverity_descriptor {
	__u8 version;
	__u8 hash_algorithm;
	__u8 log_blocksize;
	__u8 salt_size;
	__le32 sig_size;
	__le64 data_size;
	__u8 root_hash[64];
	__u8 salt[32];
	__u8 __reserved[144];
	__u8 signature[];
};

static inline bool fsverity_active(const struct inode *inode)
{ (void)inode; return false; }
static inline int fsverity_file_open(struct inode *inode, struct file *filp)
{ (void)inode; (void)filp; return 0; }
static inline int fsverity_prepare_setattr(struct dentry *dentry,
                                           struct iattr *attr)
{ (void)dentry; (void)attr; return 0; }
static inline void fsverity_cleanup_inode(struct inode *inode) { (void)inode; }
static inline int fsverity_ioctl_enable(struct file *filp, const void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fsverity_ioctl_measure(struct file *filp, void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fsverity_ioctl_read_metadata(struct file *filp,
                                               const void *uarg)
{ (void)filp; (void)uarg; return -EOPNOTSUPP; }
static inline bool fsverity_verify_blocks(struct folio *folio, size_t len,
                                          size_t offset)
{ (void)folio; (void)len; (void)offset; return false; }
static inline bool fsverity_verify_page(struct page *page)
{ (void)page; return false; }
static inline bool fsverity_verify_folio(struct folio *folio)
{ (void)folio; return false; }
static inline void fsverity_verify_bio(struct bio *bio) { (void)bio; }
static inline void fsverity_enqueue_verify_work(struct work_struct *work)
{ (void)work; }

#define FS_IOC_ENABLE_VERITY        _IOW('f', 133, struct fsverity_enable_arg)
#define FS_IOC_MEASURE_VERITY       _IOWR('f', 134, struct fsverity_digest)
#define FS_IOC_READ_VERITY_METADATA _IOWR('f', 135, struct fsverity_read_metadata_arg)

/* The largest descriptor a filesystem must be able to store. */
#define FS_VERITY_MAX_DESCRIPTOR_SIZE 16384

struct fsverity_enable_arg { __u32 version; };
struct fsverity_digest { __u16 digest_algorithm; __u16 digest_size; };
struct fsverity_read_metadata_arg { __u64 metadata_type; };

#endif
