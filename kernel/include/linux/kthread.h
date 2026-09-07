/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KTHREAD_H
#define LKPI_LINUX_KTHREAD_H
#include <lkpi/env.h>
#include <lkpi/kthread_worker.h>
#include <linux/err.h>
#include <linux/types.h>
/* Onto lkpi's kthread_worker (M101) and b1nix's kthread_create. A worker's
 * items run in submission order on a thread the caller owns. */
/* Whether the calling thread has been asked to stop. Real: btrfs's cleaner
 * and transaction threads leave their loops on it, and close_ctree waits for
 * them to. */
int lkpi_kthread_should_stop(void);
static inline bool kthread_should_stop(void) { return lkpi_kthread_should_stop() != 0; }

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
/*
 * Create a kernel thread and start it.
 *
 * btrfs runs its transaction committer and its cleaner as kthreads, and both
 * are structural rather than optional: without the committer nothing is ever
 * written to disk, and without the cleaner deleted subvolumes are never
 * reclaimed. So these are real threads on b1nix's scheduler, not stubs.
 *
 * They are stopped at unmount through kthread_stop(), which raises the
 * thread's stop flag, wakes it and waits for its function to return.
 */
/* The b1nix-side adapter, in kernel/lkpi/fs_misc.c: Linux's thread entry
 * returns int and b1nix's returns void, and creating the thread is a b1nix
 * call. */
struct lkpi_task *lkpi_fs_kthread_run(int (*threadfn)(void *data), void *data,
                                      const char *name);

struct lkpi_task *kthread_create_on_node(int (*threadfn)(void *data),
                                         void *data, int node,
                                         const char namefmt[], ...);
#define kthread_create(threadfn, data, namefmt, ...) \
	kthread_create_on_node(threadfn, data, -1, namefmt, ##__VA_ARGS__)
#define kthread_run(threadfn, data, namefmt, ...)                             \
	({                                                                        \
		struct lkpi_task *__k =                                               \
			kthread_create(threadfn, data, namefmt, ##__VA_ARGS__);           \
		if (!IS_ERR(__k))                                                     \
			wake_up_process(__k);                                             \
		__k;                                                                  \
	})
int kthread_stop(struct lkpi_task *k);
void kthread_park(struct lkpi_task *k);
void kthread_unpark(struct lkpi_task *k);
bool kthread_should_park(void);
void kthread_parkme(void);

#endif
