/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_KTHREAD_H
#define LKPI_LINUX_KTHREAD_H
#include <lkpi/env.h>
#include <lkpi/kthread_worker.h>
#include <linux/err.h>
#include <linux/types.h>
/* Onto lkpi's kthread_worker (M101) and b1nix's kthread_create. A worker's
 * items run in submission order on a thread the caller owns. */
static inline bool kthread_should_stop(void) { return false; }

/* Cancelling a queued item and waiting for any run in progress. lkpi's worker
 * has no cancel, so this flushes: the item runs once more and then the caller
 * is guaranteed it is idle, which is what the callers here need. */
#define kthread_cancel_work_sync(w)  kthread_flush_work(w)

/* sched_set_fifo lives in <linux/sched.h>; defining it here too is a
 * redefinition in every file that includes both. */

/* Linux names the worker with a format string; lkpi takes a plain name, and
 * the extra arguments describe a device the name would only decorate. */
#define kthread_create_worker(flags, namefmt, ...) \
	kthread_create_worker(namefmt)
#endif
