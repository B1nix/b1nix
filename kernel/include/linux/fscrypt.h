/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FSCRYPT_H
#define LKPI_LINUX_FSCRYPT_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>
/* struct qstr, which the inline name helpers below read. */
#include <linux/fs.h>

/*
 * Filesystem-level encryption, which b1nix does not implement.
 *
 * This mirrors upstream's own !CONFIG_FS_ENCRYPTION stubs, and the return
 * values are the contract rather than a convenience:
 *
 *   - the "prepare" hooks return 0, because an unencrypted inode is always
 *     permitted to be linked, renamed, read or opened;
 *   - the ioctls return -EOPNOTSUPP, because a userspace tool asking to set a
 *     policy on a kernel without encryption must be told no, not told yes;
 *   - `fscrypt_setup_filename` copies the name through verbatim, which is what
 *     an unencrypted directory lookup needs — returning an empty name here
 *     makes every lookup miss.
 *
 * Getting one of these backwards does not fail to compile and does not fail at
 * mount; it fails the first time someone opens a file.
 */

struct inode;
struct dentry;
struct super_block;
struct page;
struct folio;
struct bio;
struct buffer_head;
struct qstr;
struct fs_parameter;
struct seq_file;

struct fscrypt_str {
	unsigned char *name;
	u32 len;
};

struct fscrypt_name {
	const struct qstr *usr_fname;
	struct fscrypt_str disk_name;
	u32 hash;
	u32 minor_hash;
	struct fscrypt_str crypto_buf;
	bool is_nokey_name;
};

#define FSTR_INIT(n, l)      ((struct fscrypt_str){ .name = (n), .len = (l) })
#define fname_name(p)        ((p)->disk_name.name)
#define fname_len(p)         ((p)->disk_name.len)

struct fscrypt_dummy_policy {
	int dummy;
};

struct fscrypt_operations {
	int dummy;
};

struct fscrypt_policy;

static inline bool fscrypt_has_encryption_key(const struct inode *inode)
{ (void)inode; return false; }
static inline bool fscrypt_needs_contents_encryption(const struct inode *inode)
{ (void)inode; return false; }
static inline bool fscrypt_inode_uses_fs_layer_crypto(const struct inode *inode)
{ (void)inode; return false; }
static inline bool fscrypt_inode_uses_inline_crypto(const struct inode *inode)
{ (void)inode; return false; }

static inline int fscrypt_file_open(struct inode *inode, struct file *filp)
{ (void)inode; (void)filp; return 0; }
static inline int fscrypt_prepare_link(struct dentry *old_dentry,
                                       struct inode *dir,
                                       struct dentry *dentry)
{ (void)old_dentry; (void)dir; (void)dentry; return 0; }
static inline int fscrypt_prepare_rename(struct inode *old_dir,
                                         struct dentry *old_dentry,
                                         struct inode *new_dir,
                                         struct dentry *new_dentry,
                                         unsigned int flags)
{ (void)old_dir; (void)old_dentry; (void)new_dir; (void)new_dentry; (void)flags;
  return 0; }
static inline int fscrypt_prepare_lookup(struct inode *dir,
                                         struct dentry *dentry,
                                         struct fscrypt_name *fname)
{
	(void)dir; (void)dentry;
	memset(fname, 0, sizeof(*fname));
	return 0;
}
static inline int fscrypt_prepare_readdir(struct inode *dir)
{ (void)dir; return 0; }
static inline int fscrypt_prepare_setattr(struct dentry *dentry,
                                          struct iattr *attr)
{ (void)dentry; (void)attr; return 0; }
static inline int fscrypt_prepare_new_inode(struct inode *dir,
                                            struct inode *inode,
                                            bool *encrypt_ret)
{ (void)dir; (void)inode; if (encrypt_ret) *encrypt_ret = false; return 0; }
static inline int fscrypt_prepare_symlink(struct inode *dir, const char *target,
                                          unsigned int len, unsigned int max_len,
                                          struct fscrypt_str *disk_link)
{
	(void)dir; (void)max_len;
	if (len > max_len)
		return -ENAMETOOLONG;
	disk_link->name = (unsigned char *)target;
	disk_link->len = len + 1;
	return 0;
}
static inline int __fscrypt_encrypt_symlink(struct inode *inode,
                                            const char *target,
                                            unsigned int len,
                                            struct fscrypt_str *disk_link)
{ (void)inode; (void)target; (void)len; (void)disk_link; return -EOPNOTSUPP; }
static inline int fscrypt_encrypt_symlink(struct inode *inode, const char *target,
                                          unsigned int len,
                                          struct fscrypt_str *disk_link)
{ (void)inode; (void)target; (void)len; (void)disk_link; return 0; }
static inline const char *fscrypt_get_symlink(struct inode *inode,
                                              const void *caddr,
                                              unsigned int max_size,
                                              struct delayed_call *done)
{ (void)inode; (void)caddr; (void)max_size; (void)done; return ERR_PTR(-EOPNOTSUPP); }
static inline int fscrypt_symlink_getattr(const struct path *path,
                                          struct kstat *stat)
{ (void)path; (void)stat; return -EOPNOTSUPP; }

/* Name handling. Unencrypted: the disk name IS the user name. */
static inline int fscrypt_setup_filename(struct inode *dir,
                                         const struct qstr *iname,
                                         int lookup,
                                         struct fscrypt_name *fname)
{
	(void)dir; (void)lookup;
	memset(fname, 0, sizeof(*fname));
	fname->usr_fname = iname;
	fname->disk_name.name = (unsigned char *)iname->name;
	fname->disk_name.len = iname->len;
	return 0;
}
static inline void fscrypt_free_filename(struct fscrypt_name *fname)
{ (void)fname; }
static inline bool fscrypt_match_name(const struct fscrypt_name *fname,
                                      const u8 *de_name, u32 de_name_len)
{
	if (de_name_len != fname->disk_name.len)
		return false;
	return memcmp(de_name, fname->disk_name.name, de_name_len) == 0;
}
static inline u64 fscrypt_fname_siphash(const struct inode *dir,
                                        const struct qstr *name)
{ (void)dir; (void)name; return 0; }
static inline bool fscrypt_is_nokey_name(const struct dentry *dentry)
{ (void)dentry; return false; }
static inline int fscrypt_fname_alloc_buffer(u32 max_encrypted_len,
                                             struct fscrypt_str *crypto_str)
{ (void)max_encrypted_len; (void)crypto_str; return -EOPNOTSUPP; }
static inline void fscrypt_fname_free_buffer(struct fscrypt_str *crypto_str)
{ (void)crypto_str; }
static inline int fscrypt_fname_disk_to_usr(const struct inode *inode,
                                            u32 hash, u32 minor_hash,
                                            const struct fscrypt_str *iname,
                                            struct fscrypt_str *oname)
{ (void)inode; (void)hash; (void)minor_hash; (void)iname; (void)oname;
  return -EOPNOTSUPP; }

/* Contents. Nothing is ever encrypted, so nothing here is ever reached from a
 * correct caller; each returns the error that says so. */
static inline struct page *fscrypt_encrypt_pagecache_blocks(struct folio *folio,
                                                            size_t len,
                                                            size_t offs,
                                                            gfp_t gfp_flags)
{ (void)folio; (void)len; (void)offs; (void)gfp_flags; return ERR_PTR(-EOPNOTSUPP); }
static inline int fscrypt_decrypt_pagecache_blocks(struct folio *folio,
                                                   size_t len, size_t offs)
{ (void)folio; (void)len; (void)offs; return -EOPNOTSUPP; }
static inline void fscrypt_free_bounce_page(struct page *bounce_page)
{ (void)bounce_page; }
static inline bool fscrypt_is_bounce_folio(struct folio *folio)
{ (void)folio; return false; }
static inline struct folio *fscrypt_pagecache_folio(struct folio *bounce_folio)
{ (void)bounce_folio; return NULL; }
/* Returns whether the bio was decrypted. Nothing is encrypted, so nothing
 * reaches this — but the caller tests the result, and a void version is a
 * compile error at the `if`. */
static inline bool fscrypt_decrypt_bio(struct bio *bio) { (void)bio; return true; }
static inline void fscrypt_enqueue_decrypt_work(struct work_struct *work)
{ (void)work; }
static inline int fscrypt_zeroout_range(const struct inode *inode, pgoff_t lblk,
                                        sector_t pblk, unsigned int len)
{ (void)inode; (void)lblk; (void)pblk; (void)len; return -EOPNOTSUPP; }

/* Inline crypto. No device advertises it. */
static inline bool fscrypt_set_bio_crypt_ctx(struct bio *bio,
                                             const struct inode *inode,
                                             u64 first_lblk, gfp_t gfp_mask)
{ (void)bio; (void)inode; (void)first_lblk; (void)gfp_mask; return true; }
static inline bool fscrypt_set_bio_crypt_ctx_bh(struct bio *bio,
                                                const struct buffer_head *first_bh,
                                                gfp_t gfp_mask)
{ (void)bio; (void)first_bh; (void)gfp_mask; return true; }
static inline bool fscrypt_mergeable_bio(struct bio *bio,
                                         const struct inode *inode,
                                         u64 next_lblk)
{ (void)bio; (void)inode; (void)next_lblk; return true; }
static inline bool fscrypt_mergeable_bio_bh(struct bio *bio,
                                            const struct buffer_head *next_bh)
{ (void)bio; (void)next_bh; return true; }
static inline bool fscrypt_dio_supported(struct inode *inode)
{ (void)inode; return true; }
static inline u64 fscrypt_limit_io_blocks(const struct inode *inode, u64 lblk,
                                          u64 nr_blocks)
{ (void)inode; (void)lblk; return nr_blocks; }

/* Policy and keys: every ioctl is refused. */
static inline int fscrypt_ioctl_set_policy(struct file *filp, const void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fscrypt_ioctl_get_policy(struct file *filp, void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fscrypt_ioctl_get_policy_ex(struct file *filp, void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fscrypt_ioctl_get_nonce(struct file *filp, void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fscrypt_ioctl_add_key(struct file *filp, void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fscrypt_ioctl_remove_key(struct file *filp, void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fscrypt_ioctl_remove_key_all_users(struct file *filp, void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fscrypt_ioctl_get_key_status(struct file *filp, void *arg)
{ (void)filp; (void)arg; return -EOPNOTSUPP; }
static inline int fscrypt_has_permitted_context(struct inode *parent,
                                                struct inode *child)
{ (void)parent; (void)child; return 1; }
static inline int fscrypt_set_context(struct inode *inode, void *fs_data)
{ (void)inode; (void)fs_data; return -EOPNOTSUPP; }
static inline void fscrypt_put_encryption_info(struct inode *inode)
{ (void)inode; }
static inline void fscrypt_free_inode(struct inode *inode) { (void)inode; }
static inline int fscrypt_drop_inode(struct inode *inode) { (void)inode; return 0; }

/* The "dummy policy" mount option, which is a test facility upstream. Parsing
 * it has to fail rather than be ignored: a mount asking for test encryption on
 * a kernel with none must not appear to succeed. */
static inline int fscrypt_parse_test_dummy_encryption(const struct fs_parameter *param,
                                                      struct fscrypt_dummy_policy *dummy_policy)
{ (void)param; (void)dummy_policy; return -EINVAL; }
static inline bool fscrypt_dummy_policies_equal(const struct fscrypt_dummy_policy *p1,
                                                const struct fscrypt_dummy_policy *p2)
{ (void)p1; (void)p2; return true; }
static inline bool fscrypt_is_dummy_policy_set(const struct fscrypt_dummy_policy *p)
{ (void)p; return false; }
static inline void fscrypt_show_test_dummy_encryption(struct seq_file *seq,
                                                      char sep,
                                                      struct super_block *sb)
{ (void)seq; (void)sep; (void)sb; }
static inline void fscrypt_free_dummy_policy(struct fscrypt_dummy_policy *p)
{ (void)p; }
static inline void fscrypt_free(void *p) { (void)p; }

/* The ioctl numbers, which are ABI even on a kernel that refuses all of them:
 * `fscryptctl` sends exactly these, and the refusal has to be EOPNOTSUPP from
 * the right command rather than ENOTTY from an unknown one. */
#define FS_IOC_SET_ENCRYPTION_POLICY  _IOR('f', 19, struct fscrypt_policy)
#define FS_IOC_GET_ENCRYPTION_PWSALT  _IOW('f', 20, __u8[16])
#define FS_IOC_GET_ENCRYPTION_POLICY  _IOW('f', 21, struct fscrypt_policy)
#define FS_IOC_GET_ENCRYPTION_POLICY_EX _IOWR('f', 22, __u8[9])
#define FS_IOC_ADD_ENCRYPTION_KEY     _IOWR('f', 23, struct fscrypt_add_key_arg)
#define FS_IOC_REMOVE_ENCRYPTION_KEY  _IOWR('f', 24, struct fscrypt_remove_key_arg)
#define FS_IOC_REMOVE_ENCRYPTION_KEY_ALL_USERS \
	_IOWR('f', 25, struct fscrypt_remove_key_arg)
#define FS_IOC_GET_ENCRYPTION_KEY_STATUS \
	_IOWR('f', 26, struct fscrypt_get_key_status_arg)
#define FS_IOC_GET_ENCRYPTION_NONCE   _IOR('f', 27, __u8[16])

/* The largest context an encryption policy produces, which is the size of the
 * xattr a filesystem must be prepared to store. */
#define FSCRYPT_SET_CONTEXT_MAX_SIZE 40

struct fscrypt_policy { __u8 version; __u8 contents_encryption_mode; };
struct fscrypt_add_key_arg { __u32 raw_size; };
struct fscrypt_remove_key_arg { __u32 removal_status_flags; };
struct fscrypt_get_key_status_arg { __u32 status; };

#endif
