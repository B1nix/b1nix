/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_KTHREAD_WORKER_H
#define LKPI_KTHREAD_WORKER_H

#include <lkpi/types.h>

/*
 * kthread_worker — a work queue with a thread the caller owns.
 *
 * The difference from a workqueue is ownership, and it is the reason both
 * exist. A workqueue's thread is shared and anonymous; a kthread_worker's
 * belongs to one subsystem, which is what lets a driver give it a priority, pin
 * it, name it in a trace, and know that nothing else can be ahead of its work in
 * that queue. A GPU submission thread wants exactly that: everything it runs is
 * its own, in order, with no stranger's item delaying a frame.
 *
 * Items run one at a time, in submission order, in process context, so a handler
 * may sleep freely. Queuing an item that is already pending is a no-op that
 * returns 0 — the coalescing behaviour callers rely on when several events
 * should produce one run.
 *
 * kthread_queue_work() does not sleep and is safe from an interrupt handler.
 * kthread_flush_work() and kthread_destroy_worker() sleep, and neither may be
 * called from the worker's own thread: both wait for that thread to make
 * progress, so waiting from inside it would wait forever.
 */

struct kthread_work;

/*
 * Public because imported code reads `worker->task` to raise the thread's
 * priority. The rest is the queue's own bookkeeping and is not part of the
 * contract — a caller touching it is reaching past the API.
 */
struct kthread_worker {
	volatile u32 lock_word;   /* a b1nix spinlock; see <lkpi/env.h> */
	struct kthread_work *head;
	struct kthread_work *tail;
	struct kthread_work *current_work;
	volatile u32 stop;
	volatile u32 exited;
	volatile u64 executed;
	usize thread_id;
	void *task;               /* what sched_set_fifo would be handed */
};

typedef void (*kthread_work_func_t)(struct kthread_work *work);

struct kthread_work {
	kthread_work_func_t func;
	struct kthread_work *next;
	struct kthread_worker *worker;
	volatile u32 pending;
	volatile u32 running;
	volatile u64 seq; /* completed executions; lets a flush prove it waited */
};

void kthread_init_work(struct kthread_work *work, kthread_work_func_t func);

/* Create a worker and its thread. Returns NULL if the thread could not be
 * created. `name` names the thread. */
struct kthread_worker *kthread_create_worker(const char *name);

/* Queue `work`. Returns 1 if queued, 0 if it was already pending. */
int kthread_queue_work(struct kthread_worker *worker,
                       struct kthread_work *work);

/* Sleep until `work` is neither pending nor running. Returns 1 if it had to
 * wait at all. */
int kthread_flush_work(struct kthread_work *work);

/* Sleep until the backlog is empty and nothing is running. */
void kthread_flush_worker(struct kthread_worker *worker);

/* Drain the backlog, stop the thread, then free the worker. Sleeps. */
void kthread_destroy_worker(struct kthread_worker *worker);

/* Items this worker has finished. Diagnostics and self-test. */
u64 kthread_worker_executed(struct kthread_worker *worker);

#endif
