/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_QUOTAOPS_ABSENT_H
#define LKPI_QUOTAOPS_ABSENT_H

#include <linux/fs.h>
#include <lkpi/quota-absent.h>

/*
 * Quota operations, all of them absent.
 *
 * This mirrors upstream's !CONFIG_QUOTA branch, and the two conventions in it
 * are what matter:
 *
 *   - anything that CHARGES space or inodes succeeds. A filesystem calls
 *     `dquot_alloc_space` before writing and treats a failure as EDQUOT, so a
 *     stub that failed would make every write fail on a kernel with no quotas
 *     at all.
 *   - anything that turns quotas ON fails with -ENOSYS. Reporting success for
 *     `quotaon` would leave userspace believing limits were being enforced when
 *     nothing is counting.
 *
 * The `_nodirty` and `_nofail` variants exist because callers pick between them
 * deliberately, and collapsing them into one name would compile but lose the
 * distinction the next person reads the code for.
 */

static inline int dquot_initialize(struct inode *inode)
{ (void)inode; return 0; }
static inline bool dquot_initialize_needed(struct inode *inode)
{ (void)inode; return false; }
static inline void dquot_drop(struct inode *inode) { (void)inode; }
static inline struct dquot *dqget(struct super_block *sb, struct kqid qid)
{ (void)sb; (void)qid; return NULL; }
static inline void dqput(struct dquot *dquot) { (void)dquot; }

static inline int dquot_alloc_space_nodirty(struct inode *inode, qsize_t number)
{ (void)inode; (void)number; return 0; }
static inline void dquot_alloc_space_nofail(struct inode *inode, qsize_t number)
{ inode_add_bytes(inode, number); }
static inline int dquot_alloc_space(struct inode *inode, qsize_t number)
{ inode_add_bytes(inode, number); return 0; }
static inline int dquot_alloc_block_nodirty(struct inode *inode, qsize_t nr)
{ (void)inode; (void)nr; return 0; }
static inline void dquot_alloc_block_nofail(struct inode *inode, qsize_t nr)
{ inode_add_bytes(inode, nr << inode->i_blkbits); }
static inline int dquot_alloc_block(struct inode *inode, qsize_t nr)
{ inode_add_bytes(inode, nr << inode->i_blkbits); return 0; }
static inline int dquot_prealloc_block_nodirty(struct inode *inode, qsize_t nr)
{ (void)inode; (void)nr; return 0; }
static inline int dquot_prealloc_block(struct inode *inode, qsize_t nr)
{ inode_add_bytes(inode, nr << inode->i_blkbits); return 0; }
static inline int dquot_claim_block(struct inode *inode, qsize_t nr)
{ inode_add_bytes(inode, nr << inode->i_blkbits); return 0; }
static inline void dquot_reclaim_block(struct inode *inode, qsize_t nr)
{ inode_sub_bytes(inode, nr << inode->i_blkbits); }
static inline void dquot_free_space_nodirty(struct inode *inode, qsize_t nr)
{ (void)inode; (void)nr; }
static inline void dquot_free_space(struct inode *inode, qsize_t nr)
{ inode_sub_bytes(inode, nr); }
static inline void dquot_free_block_nodirty(struct inode *inode, qsize_t nr)
{ (void)inode; (void)nr; }
static inline void dquot_free_block(struct inode *inode, qsize_t nr)
{ inode_sub_bytes(inode, nr << inode->i_blkbits); }
static inline int dquot_reserve_block(struct inode *inode, qsize_t nr)
{ (void)inode; (void)nr; return 0; }
static inline void dquot_release_reservation_block(struct inode *inode,
                                                   qsize_t nr)
{ (void)inode; (void)nr; }

static inline int dquot_alloc_inode(struct inode *inode)
{ (void)inode; return 0; }
static inline void dquot_free_inode(struct inode *inode) { (void)inode; }
static inline int dquot_transfer(struct mnt_idmap *idmap, struct inode *inode,
                                 struct iattr *iattr)
{ (void)idmap; (void)inode; (void)iattr; return 0; }

static inline int dquot_file_open(struct inode *inode, struct file *file)
{ return generic_file_open(inode, file); }

/* Turning quotas on: refused, loudly. */
static inline int dquot_quota_on(struct super_block *sb, int type, int format_id,
                                 const struct path *path)
{ (void)sb; (void)type; (void)format_id; (void)path; return -ENOSYS; }
static inline int dquot_quota_on_mount(struct super_block *sb, char *qf_name,
                                       int format_id, int type)
{ (void)sb; (void)qf_name; (void)format_id; (void)type; return -ENOSYS; }
static inline int dquot_quota_off(struct super_block *sb, int type)
{ (void)sb; (void)type; return -ENOSYS; }
static inline int dquot_load_quota_inode(struct inode *inode, int type,
                                         int format_id, unsigned int flags)
{ (void)inode; (void)type; (void)format_id; (void)flags; return -ENOSYS; }
static inline int dquot_writeback_dquots(struct super_block *sb, int type)
{ (void)sb; (void)type; return 0; }
static inline int dquot_quota_sync(struct super_block *sb, int type)
{ (void)sb; (void)type; return 0; }
static inline int dquot_get_dqblk(struct super_block *sb, struct kqid id,
                                  struct qc_dqblk *di)
{ (void)sb; (void)id; (void)di; return -ENOSYS; }
static inline int dquot_get_next_dqblk(struct super_block *sb, struct kqid *id,
                                       struct qc_dqblk *di)
{ (void)sb; (void)id; (void)di; return -ENOSYS; }
static inline int dquot_set_dqblk(struct super_block *sb, struct kqid id,
                                  struct qc_dqblk *di)
{ (void)sb; (void)id; (void)di; return -ENOSYS; }
static inline int dquot_set_dqinfo(struct super_block *sb, int type,
                                   struct qc_info *ii)
{ (void)sb; (void)type; (void)ii; return -ENOSYS; }
static inline int dquot_get_state(struct super_block *sb, struct qc_state *state)
{ (void)sb; (void)state; return -ENOSYS; }
static inline int dquot_get_next_id(struct super_block *sb, struct kqid *qid)
{ (void)sb; (void)qid; return -ENOSYS; }
static inline int dquot_suspend(struct super_block *sb, int type)
{ (void)sb; (void)type; return 0; }
static inline int dquot_resume(struct super_block *sb, int type)
{ (void)sb; (void)type; return 0; }
static inline int dquot_disable(struct super_block *sb, int type,
                                unsigned int flags)
{ (void)sb; (void)type; (void)flags; return 0; }
static inline void dquot_destroy(struct dquot *dquot) { (void)dquot; }
static inline int dquot_commit(struct dquot *dquot) { (void)dquot; return 0; }
static inline int dquot_acquire(struct dquot *dquot) { (void)dquot; return 0; }
static inline int dquot_release(struct dquot *dquot) { (void)dquot; return 0; }
static inline int dquot_commit_info(struct super_block *sb, int type)
{ (void)sb; (void)type; return 0; }
static inline int dquot_mark_dquot_dirty(struct dquot *dquot)
{ (void)dquot; return 0; }
static inline struct dquot *dquot_alloc(struct super_block *sb, int type)
{ (void)sb; (void)type; return NULL; }
static inline struct inode *dquot_to_inode(struct dquot *dquot)
{ (void)dquot; return NULL; }

static inline unsigned int sb_dqopt_flags(struct super_block *sb)
{ (void)sb; return 0; }
#define sb_dqopt(sb) (&(sb)->s_dquot)
#define sb_has_quota_usage_enabled(sb, type) 0
#define sb_has_quota_limits_enabled(sb, type) 0
#define sb_has_quota_suspended(sb, type) 0
#define sb_has_quota_active(sb, type) 0
#define sb_any_quota_loaded(sb) 0
#define sb_any_quota_suspended(sb) 0

extern const struct dquot_operations dquot_operations;
extern const struct quotactl_ops dquot_quotactl_sysfile_ops;

#endif
