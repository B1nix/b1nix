/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_WRITEBACK_H
#define LKPI_LINUX_WRITEBACK_H

#include <linux/types.h>
#include <linux/list.h>

/*
 * What a writeback pass is being asked to do.
 *
 * The structure is passed down from the flusher into the filesystem's
 * `writepages`, and the filesystem both reads it and writes back to it. Three
 * fields carry most of the meaning:
 *
 *   nr_to_write   a budget, decremented as pages are written. A filesystem that
 *                 ignores it starves every other inode on the device.
 *   sync_mode     WB_SYNC_ALL means the caller will wait and every page must be
 *                 written; WB_SYNC_NONE means best effort. fsync uses the first
 *                 and background flushing the second, and treating them alike
 *                 either makes fsync lose data or makes background writeback
 *                 block.
 *   range_start/end  the byte range, when the caller wants only part of a file.
 *                 `range_cyclic` says instead "resume where the last pass
 *                 stopped", which is what background writeback wants.
 */

struct bdi_writeback;
struct inode;
struct bio;
struct folio;
struct address_space;
struct cgroup_subsys_state;

enum writeback_sync_modes {
	WB_SYNC_NONE,   /* best effort; do not block */
	WB_SYNC_ALL,    /* wait for everything */
};

enum wb_reason {
	WB_REASON_BACKGROUND,
	WB_REASON_VMSCAN,
	WB_REASON_SYNC,
	WB_REASON_PERIODIC,
	WB_REASON_LAPTOP_TIMER,
	WB_REASON_FS_FREE_SPACE,
	WB_REASON_FORKER_THREAD,
	WB_REASON_FOREIGN_FLUSH,
	WB_REASON_MAX,
};

struct writeback_control {
	long nr_to_write;
	long pages_skipped;
	loff_t range_start;
	loff_t range_end;
	enum writeback_sync_modes sync_mode;
	unsigned for_kupdate : 1;
	unsigned for_background : 1;
	unsigned tagged_writepages : 1;
	unsigned for_reclaim : 1;
	unsigned range_cyclic : 1;
	unsigned for_sync : 1;
	unsigned unpinned_fscache_wb : 1;
	/*
	 * "Do not attribute these writes to a cgroup."
	 *
	 * btrfs sets it when a worker thread is writing pages another task
	 * dirtied: the worker is not the owner, and attributing the I/O to it
	 * would throttle the wrong cgroup. There is no block cgroup controller
	 * here, so nothing acts on it — but the field must exist, because btrfs
	 * sets it by designated initialiser and a missing field is a compile
	 * error at the initialiser rather than an ignored hint.
	 */
	unsigned no_cgroup_owner : 1;
	unsigned punt_to_cgroup : 1;
};

/*
 * Cgroup attribution of writeback. b1nix has no block cgroup controller, so
 * these record nothing — see <linux/blk-cgroup.h> for why that is the honest
 * behaviour rather than a gap.
 */
static inline void wbc_init_bio(struct writeback_control *wbc, struct bio *bio)
{ (void)wbc; (void)bio; }
static inline void wbc_attach_and_unlock_inode(struct writeback_control *wbc,
                                               struct inode *inode)
{ (void)wbc; (void)inode; }
static inline void wbc_attach_fdatawrite_inode(struct writeback_control *wbc,
                                               struct inode *inode)
{ (void)wbc; (void)inode; }
static inline void wbc_detach_inode(struct writeback_control *wbc)
{ (void)wbc; }
static inline void wbc_account_cgroup_owner(struct writeback_control *wbc,
                                            struct page *page, size_t bytes)
{ (void)wbc; (void)page; (void)bytes; }
static inline struct cgroup_subsys_state *
wbc_blkcg_css(struct writeback_control *wbc)
{ (void)wbc; return NULL; }

/*
 * The bio flags implied by a writeback request: REQ_SYNC when the caller is
 * waiting, plus the background/idle hints. A submission that dropped them turns
 * every fsync into background writeback, which completes eventually rather than
 * now.
 */
unsigned int wbc_to_write_flags(struct writeback_control *wbc);

/* Which inode a flusher should write next, and the balance-dirty throttle. */
void balance_dirty_pages_ratelimited(struct address_space *mapping);
int balance_dirty_pages_ratelimited_flags(struct address_space *mapping,
                                          unsigned int flags);
void inode_attach_wb(struct inode *inode, struct folio *folio);
void wakeup_flusher_threads(enum wb_reason reason);
void writeback_inodes_sb(struct super_block *sb, enum wb_reason reason);
/* Start writeback only if a flush is not already running. ext4 calls it when it
 * is short of space: waiting behind an in-progress flush would be a second
 * queue for the same work. */
void try_to_writeback_inodes_sb(struct super_block *sb, enum wb_reason reason);

/* Dirty-throttle flags. BDP_ASYNC says the caller cannot block, so the
 * throttle reports that it would have waited instead of waiting. */
#define BDP_ASYNC (1 << 0)
int write_cache_pages(struct address_space *mapping,
                      struct writeback_control *wbc,
                      int (*writepage)(struct folio *, struct writeback_control *,
                                       void *),
                      void *data);
void __inode_attach_wb(struct inode *inode, struct folio *folio);

#endif
