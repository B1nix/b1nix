/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_WORKQUEUE_H
#define LKPI_LINUX_WORKQUEUE_H
#include <lkpi/workqueue.h>
#include <lkpi/kthread_worker.h>
#include <linux/kernel.h>
/* Onto lkpi's workqueues (M99): one kthread per queue, FIFO, handlers in
 * process context so they may sleep. The names already match. */
#define create_singlethread_workqueue(name) alloc_workqueue(name)
#define WQ_UNBOUND 0
/* Linux keeps a second shared queue for items that may run long, so they do not
 * delay short ones. lkpi has one shared queue; pointing both names at it means
 * a long item can delay a short one, which is a latency difference and not a
 * correctness one. */
/* Linux keeps queues with different concurrency rules; lkpi has one. Pointing
 * every name at it means less parallelism between items, never less
 * correctness — they still run in submission order on a real thread. */
/* Re-arm a delayed item to a new deadline, cancelling any pending one. */
#define mod_delayed_work(wq, dwork, delay)             \
	({                                                 \
		cancel_delayed_work(dwork);                    \
		queue_delayed_work((wq), (dwork), (delay));    \
	})

#define to_delayed_work(w) container_of(w, struct delayed_work, work)

/* Named as values, because imported code passes them where a pointer is
 * expected rather than calling them. They resolve to lkpi's accessor under its
 * own name: a macro that expanded to a call of the same spelling worked for the
 * one name the preprocessor refuses to expand twice, and turned every alias of
 * it into `(system_wq())()`. */
#define system_wq         (lkpi_system_wq())
#define system_unbound_wq (lkpi_system_wq())
#define system_highpri_wq (lkpi_system_wq())
#define system_long_wq    (lkpi_system_wq())
/* A work item living on the caller's stack. b1nix tracks no per-item debug
 * state, so it initialises exactly like any other. */
#define INIT_WORK_ONSTACK(w, f)      INIT_WORK(w, f)
#define destroy_work_on_stack(w)     do { (void)(w); } while (0)
#define cancel_work_sync(w)         flush_work(w)
#define cancel_delayed_work_sync(d) (cancel_delayed_work(d), flush_work(&(d)->work))
#define flush_delayed_work(d)       flush_work(&(d)->work)
#define schedule_delayed_work(d, t) queue_delayed_work(system_wq, (d), (t))

/* The item a worker is running right now, or NULL. lkpi's shared queue does
 * not publish it, and the caller uses it only to ask "am I inside my own
 * handler" — answering NULL means it concludes it is not, which is the safe
 * direction: it takes the path that waits rather than the one that assumes. */
static inline struct work_struct *current_work(void) { return 0; }
#endif
