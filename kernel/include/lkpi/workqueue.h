/* SPDX-License-Identifier: MIT */
#ifndef LKPI_WORKQUEUE_H
#define LKPI_WORKQUEUE_H

#include <b1nix/spinlock.h>
#include <lkpi/completion.h>
#include <lkpi/types.h>

/*
 * workqueue — deferred work run in a kernel thread.
 *
 * A workqueue owns one kthread (created with kthread_create) and a FIFO of
 * pending items. Handlers therefore run in process context, one at a time, in
 * submission order — the guarantee GPU drivers rely on when they push a reset
 * or a hotplug handler out of an interrupt.
 *
 * queue_work() itself never sleeps and is safe from an interrupt handler; the
 * *handler* may sleep freely.
 *
 * A work item may be re-queued from its own handler. Queuing an item that is
 * already pending is a no-op that returns 0, matching what callers expect from
 * "coalesce this request".
 */

struct workqueue_struct;
struct work_struct;

typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	work_func_t func;
	struct work_struct *next;    /* queue linkage */
	struct workqueue_struct *wq; /* queue it is pending on, else NULL */
	volatile u32 pending;
	volatile u32 running;
	volatile u64 seq;            /* completed-execution counter */
};

/* Delayed work: queued onto its workqueue once `delay` scheduler ticks have
 * elapsed. The delay is serviced by the queue's own thread, so a delayed item
 * costs no extra thread. */
struct delayed_work {
	struct work_struct work;
	u64 due_tick;
	struct delayed_work *next;
	struct workqueue_struct *wq;
	volatile u32 armed;
};

void INIT_WORK(struct work_struct *work, work_func_t func);
void INIT_DELAYED_WORK(struct delayed_work *dwork, work_func_t func);

/* Create a queue with its own kthread. `name` is used for the thread name.
 * Returns NULL if the thread could not be created. */
struct workqueue_struct *alloc_workqueue(const char *name);

/* Stop the queue's thread once its backlog has drained, then free it. Sleeps. */
void destroy_workqueue(struct workqueue_struct *wq);

/* Queue `work`. Returns 1 if it was queued, 0 if it was already pending. */
int queue_work(struct workqueue_struct *wq, struct work_struct *work);

/* Queue `work` after `delay` scheduler ticks (10 ms each). Returns 1 if armed,
 * 0 if it was already pending or armed. */
int queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
                       u64 delay_ticks);

/* Cancel a delayed item that has not fired. Returns 1 if it was disarmed. */
int cancel_delayed_work(struct delayed_work *dwork);

/* Sleep until `work` is neither pending nor running. Returns 1 if it had to
 * wait. Must not be called from the queue's own thread. */
int flush_work(struct work_struct *work);

/* Sleep until the queue's backlog is empty and nothing is running. */
void flush_workqueue(struct workqueue_struct *wq);

/* The shared queue every driver may use for short, non-blocking work. Created
 * lazily on first use; NULL only if the kthread could not be created. */
struct workqueue_struct *system_wq(void);
static inline int schedule_work(struct work_struct *work)
{
	struct workqueue_struct *wq = system_wq();
	return wq ? queue_work(wq, work) : 0;
}

#endif
