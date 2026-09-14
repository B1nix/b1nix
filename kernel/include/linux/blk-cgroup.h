/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BLK_CGROUP_H
#define LKPI_LINUX_BLK_CGROUP_H

/*
 * Block I/O cgroups: attributing an I/O to the control group that caused it, so
 * a throttle can be applied to it. b1nix has cgroups but no block controller,
 * and there is nothing below to throttle against.
 *
 * btrfs uses these to keep an I/O attributed to the process that dirtied the
 * page rather than to the worker thread that eventually writes it — the
 * "cgroup punt" path. With no controller, the attribution has no consumer, so
 * the associate calls do nothing and `bio_blkcg_css` reports no association,
 * which is what sends btrfs down its ordinary submission path rather than the
 * punt.
 */

struct bio;
struct cgroup_subsys_state;
struct block_device;

static inline void bio_associate_blkg_from_css(struct bio *bio,
                                               struct cgroup_subsys_state *css)
{ (void)bio; (void)css; }
static inline struct cgroup_subsys_state *bio_blkcg_css(struct bio *bio)
{ (void)bio; return NULL; }
/*
 * The "punt" is a cgroup detour, not a discard.
 *
 * btrfs submits every bio from an async helper through this — the flag is
 * REQ_BTRFS_CGROUP_PUNT — so that the I/O is accounted to the cgroup that
 * dirtied the page rather than to the worker. Without cgroups the bio is
 * simply submitted, which is exactly what upstream's !CONFIG_BLK_CGROUP
 * version does. A no-op here THREW THE WRITE AWAY: the page stayed under
 * writeback forever and the next write to the same file waited on it.
 */
void submit_bio(struct bio *bio);
static inline void blkcg_punt_bio_submit(struct bio *bio) { submit_bio(bio); }
static inline void blk_cgroup_bio_start(struct bio *bio) { (void)bio; }
static inline bool blk_cgroup_congested(void) { return false; }
static inline void blkcg_schedule_throttle(struct block_device *bdev,
                                           bool use_memdelay)
{ (void)bdev; (void)use_memdelay; }

/*
 * Reference counting on a cgroup subsystem state.
 *
 * btrfs takes one when it hands work to a thread that must be billed to the
 * caller's cgroup, and drops it when the work is done. With no controller there
 * is nothing to reference, and the calls are the pairing rather than the
 * counting — but they must exist, because they appear on both sides of a
 * conditional and an unbalanced pair would be invisible.
 */
struct cgroup_subsys_state;
static inline void css_get(struct cgroup_subsys_state *css) { (void)css; }
static inline void css_put(struct cgroup_subsys_state *css) { (void)css; }
extern struct cgroup_subsys_state * const blkcg_root_css;

struct lkpi_task;
static inline void kthread_associate_blkcg(struct cgroup_subsys_state *css)
{ (void)css; }

#endif
