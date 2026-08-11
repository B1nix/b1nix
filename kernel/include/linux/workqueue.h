/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_WORKQUEUE_H
#define LKPI_LINUX_WORKQUEUE_H
#include <lkpi/workqueue.h>
#include <lkpi/kthread_worker.h>
#include <linux/kernel.h>
/* Onto lkpi's workqueues (M99): one kthread per queue, FIFO, handlers in
 * process context so they may sleep. The names already match. */
#define create_singlethread_workqueue(name) alloc_workqueue(name, 0, 1)
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

/* Whether the item is queued and has not started yet. Read without the queue's
 * lock, as upstream's is — the answer is a hint, and the caller that needs
 * certainty flushes instead. */
#define work_pending(work) ((work)->pending != 0)
#define delayed_work_pending(dw) work_pending(&(dw)->work)


/* A queue that runs its items one at a time, in order. b1nix's workqueues all
 * do — each owns a single thread — so "ordered" is what every queue here
 * already is, and this is the plain constructor under the name that says the
 * caller depends on that property. */
/* Every queue here owns one thread and runs its items in order, so "ordered" is
 * what they all already are. */
#define alloc_ordered_workqueue(fmt, flags, ...) alloc_workqueue(fmt, flags, 1)
#define WQ_MEM_RECLAIM 0
#define WQ_HIGHPRI     0
#define WQ_FREEZABLE   0


/* Run the queue dry, including anything the running items queue themselves.
 * flush_workqueue only waits for what was already pending, which is why
 * upstream keeps the two apart — a teardown needs this one. */
void drain_workqueue(struct workqueue_struct *wq);


/* The concurrency cap an unbound queue is created with. Every queue here owns
 * one thread, so the cap describes a parallelism that does not exist — it is
 * accepted and has no effect. */
#define WQ_UNBOUND_MAX_ACTIVE 512
#define WQ_UNBOUND            0
#define WQ_SYSFS              0


/*
 * Work that runs after a grace period.
 *
 * The point is ordering: the item must not run while a reader could still be
 * looking at what it is about to free. b1nix's synchronize_rcu is a real wait,
 * so this waits and then queues — the same order, paid for by the caller of
 * queue_rcu_work rather than by a callback thread.
 */
struct rcu_work {
	struct work_struct work;
	struct rcu_head rcu;
};

#define INIT_RCU_WORK(rwork, func) INIT_WORK(&(rwork)->work, func)
bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rwork);


/* Initialising a delayed work item that lives on the stack, and tearing it
 * down. The distinction upstream is debugobjects tracking, which is absent here
 * — see <linux/debugobjects.h> — so these are the ordinary init and nothing. */
#define INIT_DELAYED_WORK_ONSTACK(dwork, func) INIT_DELAYED_WORK(dwork, func)
static inline void destroy_delayed_work_on_stack(struct delayed_work *work)
{ (void)work; }

#endif
