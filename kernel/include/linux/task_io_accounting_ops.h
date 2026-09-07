/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TASK_IO_ACCOUNTING_OPS_H
#define LKPI_LINUX_TASK_IO_ACCOUNTING_OPS_H

/*
 * Per-task I/O accounting — the read_bytes/write_bytes in /proc/<pid>/io.
 * b1nix does not keep them, so the filesystem's contributions go nowhere.
 *
 * `task_io_account_cancelled_write` is the one with a subtlety worth keeping in
 * the name: it exists to SUBTRACT bytes that were counted as written and then
 * were not, and a version that added them would make the counter drift upward
 * on every truncated write. Since nothing is counted, nothing drifts.
 */

static inline void task_io_account_read(size_t bytes) { (void)bytes; }
static inline void task_io_account_write(size_t bytes) { (void)bytes; }
static inline void task_io_account_cancelled_write(size_t bytes) { (void)bytes; }
static inline unsigned long task_io_get_inblock(const struct task_struct *p)
{ (void)p; return 0; }
static inline unsigned long task_io_get_oublock(const struct task_struct *p)
{ (void)p; return 0; }

#endif
