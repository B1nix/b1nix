/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SCHED_MM_H
#define LKPI_LINUX_SCHED_MM_H
#include <linux/gfp.h>

/*
 * Allocation-context scopes.
 *
 * On Linux these mask out reclaim for a region of code that must not re-enter
 * the filesystem or the shrinker — a driver holding a lock reclaim would try to
 * take. b1nix's allocator never reclaims and never sleeps, so there is nothing
 * to mask: the property these scopes buy is one b1nix has unconditionally. They
 * are accepted and discarded rather than reimplemented, and the flags returned
 * are the ones the caller must hand back, so nesting still balances.
 */
static inline unsigned int memalloc_noreclaim_save(void) { return 0; }
static inline void memalloc_noreclaim_restore(unsigned int flags) { (void)flags; }
static inline unsigned int memalloc_nofs_save(void) { return 0; }
static inline void memalloc_nofs_restore(unsigned int flags) { (void)flags; }
static inline unsigned int memalloc_noio_save(void) { return 0; }
static inline void memalloc_noio_restore(unsigned int flags) { (void)flags; }
#endif
