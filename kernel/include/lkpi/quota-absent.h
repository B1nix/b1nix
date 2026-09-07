/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_QUOTA_ABSENT_H
#define LKPI_QUOTA_ABSENT_H

#include <linux/types.h>
#include <linux/list.h>

/*
 * Disk quotas — the types, not the implementation.
 *
 * b1nix accounts no quotas and `CONFIG_QUOTA` is off, but the types cannot
 * simply be absent: ext4's superblock structure carries `struct quota_info`
 * unconditionally, its operations vector has `dquot_operations *` in it, and
 * `qsize_t` appears in function signatures that are compiled either way. So the
 * shapes exist and the operations in <linux/quotaops.h> are stubs.
 *
 * The `MAXQUOTAS` layout is upstream's because ext4 indexes arrays with
 * `USRQUOTA`/`GRPQUOTA`/`PRJQUOTA` directly.
 */

typedef long long qsize_t;

#define USRQUOTA 0
#define GRPQUOTA 1
#define PRJQUOTA 2
#define MAXQUOTAS 3

#define QTYPE_MASK_USR (1 << USRQUOTA)
#define QTYPE_MASK_GRP (1 << GRPQUOTA)
#define QTYPE_MASK_PRJ (1 << PRJQUOTA)

/* Superblock quota flags. */
#define DQUOT_USAGE_ENABLED  0x01
#define DQUOT_LIMITS_ENABLED 0x02
#define DQUOT_SUSPENDED      0x04
#define DQUOT_QUOTA_SYS_FILE 0x08
#define DQUOT_NEGATIVE_USAGE 0x10
#define DQUOT_NOLIST_DIRTY   0x20

#define DQUOT_STATE_FLAGS (DQUOT_USAGE_ENABLED | DQUOT_LIMITS_ENABLED | \
                           DQUOT_SUSPENDED)
#define DQUOT_STATE_LAST 3
#define _DQUOT_STATE_FLAGS DQUOT_STATE_FLAGS

/* Which of a quota's limits an operation is allowed to exceed. */
#define DQUOT_SPACE_WARN    0x1
#define DQUOT_SPACE_RESERVE 0x2
#define DQUOT_SPACE_NOFAIL  0x4

struct kqid { int type; unsigned int id; };
struct kprojid { unsigned int val; };

struct mem_dqblk {
	qsize_t dqb_bhardlimit;
	qsize_t dqb_bsoftlimit;
	qsize_t dqb_curspace;
	qsize_t dqb_rsvspace;
	qsize_t dqb_ihardlimit;
	qsize_t dqb_isoftlimit;
	qsize_t dqb_curinodes;
	time64_t dqb_btime;
	time64_t dqb_itime;
};

struct dquot {
	struct list_head dq_free;
	struct list_head dq_inuse;
	struct list_head dq_dirty;
	atomic_t dq_count;
	struct super_block *dq_sb;
	struct kqid dq_id;
	loff_t dq_off;
	unsigned long dq_flags;
	struct mem_dqblk dq_dqb;
};

struct qc_dqblk;
struct qc_state;
struct qc_info;
struct qc_type_state;
struct path;

struct dquot_operations {
	int (*write_dquot)(struct dquot *);
	struct dquot *(*alloc_dquot)(struct super_block *, int);
	void (*destroy_dquot)(struct dquot *);
	int (*acquire_dquot)(struct dquot *);
	int (*release_dquot)(struct dquot *);
	int (*mark_dirty)(struct dquot *);
	int (*write_info)(struct super_block *, int);
	qsize_t *(*get_reserved_space)(struct inode *);
	int (*get_projid)(struct inode *, struct kprojid *);
	int (*get_inode_usage)(struct inode *, qsize_t *);
	int (*get_next_id)(struct super_block *, struct kqid *);
};

struct quotactl_ops {
	int (*quota_on)(struct super_block *, int, int, const struct path *);
	int (*quota_off)(struct super_block *, int);
	int (*quota_enable)(struct super_block *, unsigned int);
	int (*quota_disable)(struct super_block *, unsigned int);
	int (*quota_sync)(struct super_block *, int);
	int (*set_info)(struct super_block *, int, struct qc_info *);
	int (*get_dqblk)(struct super_block *, struct kqid, struct qc_dqblk *);
	int (*get_nextdqblk)(struct super_block *, struct kqid *,
	                     struct qc_dqblk *);
	int (*set_dqblk)(struct super_block *, struct kqid, struct qc_dqblk *);
	int (*get_state)(struct super_block *, struct qc_state *);
	int (*rm_xquota)(struct super_block *, unsigned int);
};

struct quota_format_type {
	int qf_fmt_id;
	const struct quota_format_ops *qf_ops;
	struct module *qf_owner;
	struct quota_format_type *qf_next;
};

struct mem_dqinfo {
	struct quota_format_type *dqi_format;
	int dqi_fmt_id;
	struct list_head dqi_dirty_list;
	unsigned long dqi_flags;
	unsigned int dqi_bgrace;
	unsigned int dqi_igrace;
	qsize_t dqi_max_spc_limit;
	qsize_t dqi_max_ino_limit;
	void *dqi_priv;
};

struct quota_info {
	unsigned int flags;
	struct rw_semaphore dqio_sem;
	struct inode *files[MAXQUOTAS];
	struct mem_dqinfo info[MAXQUOTAS];
	const struct quota_format_ops *ops[MAXQUOTAS];
};

struct qc_dqblk {
	int d_fieldmask;
	u64 d_spc_hardlimit;
	u64 d_spc_softlimit;
	u64 d_ino_hardlimit;
	u64 d_ino_softlimit;
	u64 d_space;
	u64 d_ino_count;
	s64 d_ino_timer;
	s64 d_spc_timer;
	int d_ino_warns;
	int d_spc_warns;
	u64 d_rt_spc_hardlimit;
	u64 d_rt_spc_softlimit;
	u64 d_rt_space;
	s64 d_rt_spc_timer;
	int d_rt_spc_warns;
};

struct qc_type_state {
	unsigned int flags;
	unsigned int spc_timelimit;
	unsigned int ino_timelimit;
	unsigned int rt_spc_timelimit;
	unsigned int spc_warnlimit;
	unsigned int ino_warnlimit;
	unsigned int rt_spc_warnlimit;
	unsigned long long ino;
	blkcnt_t blocks;
	blkcnt_t nextents;
};

struct qc_state {
	unsigned int s_incoredqs;
	struct qc_type_state s_state[MAXQUOTAS];
};

struct qc_info {
	int i_fieldmask;
	unsigned int i_flags;
	unsigned int i_spc_timelimit;
	unsigned int i_ino_timelimit;
	unsigned int i_rt_spc_timelimit;
	unsigned int i_spc_warnlimit;
	unsigned int i_ino_warnlimit;
	unsigned int i_rt_spc_warnlimit;
};

#define QC_SPC_TIMER   (1 << 0)
#define QC_INO_TIMER   (1 << 1)
#define QC_SPC_WARNS   (1 << 2)
#define QC_INO_WARNS   (1 << 3)
#define QC_FLAGS       (1 << 4)

static inline struct kqid make_kqid(struct user_namespace *from, int type,
                                    unsigned int id)
{
	struct kqid kqid = { .type = type, .id = id };

	(void)from;
	return kqid;
}

static inline bool qid_valid(struct kqid qid) { return qid.id != (unsigned)-1; }
static inline bool qid_eq(struct kqid a, struct kqid b)
{ return a.type == b.type && a.id == b.id; }

/*
 * Project ids: a third quota dimension alongside user and group, and the one
 * ext4 stores in the inode. Same struct-wrapper reasoning as kuid_t — an id
 * that has not been through a conversion cannot be assigned by accident.
 */
/*
 * The RAW project id, not the wrapper.
 *
 * `kprojid_t` is the kernel's checked form and `projid_t` is the number as it
 * sits on disk — ext4 reads one out of the inode, compares it against a
 * constant and converts it with make_kprojid. Making them the same type breaks
 * every one of those, and defeats the reason the wrapper exists.
 */
typedef unsigned int projid_t;
#define INVALID_PROJID ((kprojid_t){ (unsigned int)-1 })
#define KPROJIDT_INIT(value) ((kprojid_t){ (unsigned int)(value) })

static inline unsigned int __kprojid_val(kprojid_t projid) { return projid.val; }
static inline bool projid_eq(kprojid_t a, kprojid_t b)
{ return a.val == b.val; }
static inline bool projid_valid(kprojid_t projid)
{ return projid.val != (unsigned int)-1; }
static inline kprojid_t make_kprojid(struct user_namespace *from, unsigned int p)
{ (void)from; return KPROJIDT_INIT(p); }
static inline unsigned int from_kprojid(struct user_namespace *to, kprojid_t p)
{ (void)to; return p.val; }
static inline unsigned int from_kprojid_munged(struct user_namespace *to,
                                               kprojid_t p)
{ (void)to; return p.val; }

/* The on-disk quota file formats. The numbers are what a quota file's header
 * records, so they are ABI with the userspace tools. */
#define QFMT_VFS_OLD 1
#define QFMT_VFS_V0  2
#define QFMT_OCFS2   3
#define QFMT_VFS_V1  4

struct inode;
struct iattr;
/* Does this attribute change move space between quotas? Only then does the
 * filesystem have to open a transaction big enough for the transfer. */
bool is_quota_modification(struct mnt_idmap *idmap, struct inode *inode,
                           struct iattr *ia);

#endif
