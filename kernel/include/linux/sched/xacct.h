/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_SCHED_XACCT_H
#define LKPI_LINUX_SCHED_XACCT_H

#include <linux/task_io_accounting_ops.h>

/*
 * Extended per-task accounting: the character and syscall counters that feed
 * taskstats. b1nix keeps none of them, and the read/write byte counters this
 * header's callers use live in <linux/task_io_accounting_ops.h>, which is where
 * the no-ops are.
 */

static inline void add_rchar(struct task_struct *tsk, ssize_t amt)
{ (void)tsk; (void)amt; }
static inline void add_wchar(struct task_struct *tsk, ssize_t amt)
{ (void)tsk; (void)amt; }
static inline void inc_syscr(struct task_struct *tsk) { (void)tsk; }
static inline void inc_syscw(struct task_struct *tsk) { (void)tsk; }

#endif
