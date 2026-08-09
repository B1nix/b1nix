/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_SRCU_H
#define LKPI_LINUX_SRCU_H
#include <lkpi/rcu.h>
#include <linux/types.h>
/*
 * Sleepable RCU. lkpi's RCU is already built on the two-bucket scheme SRCU
 * uses, but its read sections disable interrupts and therefore cannot sleep —
 * which is the one thing SRCU exists to allow.
 *
 * So this maps onto it honestly and says what that costs: an srcu read section
 * here behaves exactly like an rcu one, and a caller that sleeps inside it is a
 * bug that b1nix's never-sleep rule will catch, rather than a supported use. No
 * imported code in the DRM core sleeps inside one today; the day one does, this
 * needs a real per-domain implementation rather than a wider comment.
 */
struct srcu_struct { int unused; };
static inline int init_srcu_struct(struct srcu_struct *s) { (void)s; return 0; }
static inline void cleanup_srcu_struct(struct srcu_struct *s) { (void)s; }
static inline int srcu_read_lock(struct srcu_struct *s)
{ (void)s; rcu_read_lock(); return 0; }
static inline void srcu_read_unlock(struct srcu_struct *s, int idx)
{ (void)s; (void)idx; rcu_read_unlock(); }
/* A file-scope srcu domain. One global instance per name; nothing here is
 * per-domain, so the object exists only to be addressable. */
#define DEFINE_STATIC_SRCU(name) static struct srcu_struct name

static inline void synchronize_srcu(struct srcu_struct *s)
{ (void)s; synchronize_rcu(); }
#endif
