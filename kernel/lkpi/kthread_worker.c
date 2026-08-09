/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: kthread_worker.
 *
 * A FIFO and one thread that drains it. The thread parks on the worker's own
 * address as a wait channel through the scheduler's two-phase wait, so a queue
 * arriving between "the list is empty" and the park is not lost — the same race
 * every other sleeping primitive in this layer has to close.
 *
 * Ordering is a property callers depend on, so it is stated plainly: items are
 * appended at the tail and taken from the head, one at a time, by a single
 * thread. Nothing here can reorder them.
 */

#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/tlb.h>
#include <lkpi/kthread_worker.h>

void kthread_init_work(struct kthread_work *work, kthread_work_func_t func)
{
	if (!work)
		return;
	work->func = func;
	work->next = 0;
	work->worker = 0;
	work->pending = 0;
	work->running = 0;
	work->seq = 0;
}

static void kthread_worker_fn(void *arg)
{
	struct kthread_worker *worker = (struct kthread_worker *)arg;

	for (;;) {
		u64 flags;
		spin_lock_irqsave((spinlock_t *)&worker->lock_word, &flags);
		struct kthread_work *work = worker->head;
		if (work) {
			worker->head = work->next;
			if (!worker->head)
				worker->tail = 0;
			work->next = 0;
			work->pending = 0;
			work->running = 1;
			worker->current_work = work;
		}
		u32 stop = worker->stop;
		spin_unlock_irqrestore((spinlock_t *)&worker->lock_word, flags);

		if (work) {
			/* Outside the lock: the handler may sleep, and holding an
			 * interrupt-disabling spinlock across it would break the rule that
			 * makes every other lock in the kernel safe. */
			if (work->func)
				work->func(work);

			spin_lock_irqsave((spinlock_t *)&worker->lock_word, &flags);
			work->running = 0;
			work->seq++;
			worker->current_work = 0;
			worker->executed++;
			spin_unlock_irqrestore((spinlock_t *)&worker->lock_word, flags);
			/* Wake whoever is flushing this item or the whole worker. */
			scheduler_wake_all(worker);
			continue;
		}

		/* Nothing to run. Stop only once the backlog is genuinely empty, so a
		 * destroy cannot drop work that was already queued. */
		if (stop)
			break;

		scheduler_wait_prepare(worker);
		spin_lock_irqsave((spinlock_t *)&worker->lock_word, &flags);
		int idle = (worker->head == 0 && worker->stop == 0);
		spin_unlock_irqrestore((spinlock_t *)&worker->lock_word, flags);
		if (idle)
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}

	worker->exited = 1;
	scheduler_wake_all(worker);
	scheduler_exit_current(0);
}

struct kthread_worker *kthread_create_worker(const char *name)
{
	struct kthread_worker *worker = (struct kthread_worker *)lkpi_kcalloc(
		1, sizeof(struct kthread_worker), GFP_KERNEL);
	if (!worker)
		return 0;
	worker->lock_word = SPINLOCK_INIT;

	int tid = kthread_create(name ? name : "lkpi-worker", kthread_worker_fn,
	                         worker);
	if (tid < 0) {
		lkpi_kfree(worker);
		return 0;
	}
	worker->thread_id = (usize)tid;
	return worker;
}

int kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work)
{
	if (!worker || !work)
		return 0;

	u64 flags;
	spin_lock_irqsave((spinlock_t *)&worker->lock_word, &flags);
	if (work->pending) {
		/* Already queued: coalesce rather than run it twice. */
		spin_unlock_irqrestore((spinlock_t *)&worker->lock_word, flags);
		return 0;
	}
	work->pending = 1;
	work->worker = worker;
	work->next = 0;
	if (worker->tail)
		worker->tail->next = work;
	else
		worker->head = work;
	worker->tail = work;
	spin_unlock_irqrestore((spinlock_t *)&worker->lock_word, flags);

	scheduler_wake_all(worker);
	return 1;
}

/* Park until `cond` holds, re-testing under the worker's lock. Shared by both
 * flushes so the wait protocol exists in one place. */
static int kthread_wait_until_idle(struct kthread_worker *worker,
                                   struct kthread_work *work)
{
	int waited = 0;
	for (;;) {
		u64 flags;
		spin_lock_irqsave((spinlock_t *)&worker->lock_word, &flags);
		int busy;
		if (work)
			busy = (work->pending || work->running);
		else
			busy = (worker->head != 0 || worker->current_work != 0);
		spin_unlock_irqrestore((spinlock_t *)&worker->lock_word, flags);
		if (!busy)
			return waited;

		waited = 1;
		if (!scheduler_can_block()) {
			/* Cannot park: spin, and keep servicing shootdowns. Flushing from
			 * such a context is a caller bug, but wedging the CPU reports it
			 * worse than making progress does. */
			__asm__ volatile("pause");
			tlb_shootdown_poll();
			continue;
		}

		scheduler_wait_prepare(worker);
		spin_lock_irqsave((spinlock_t *)&worker->lock_word, &flags);
		int still;
		if (work)
			still = (work->pending || work->running);
		else
			still = (worker->head != 0 || worker->current_work != 0);
		spin_unlock_irqrestore((spinlock_t *)&worker->lock_word, flags);
		if (still)
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}
}

int kthread_flush_work(struct kthread_work *work)
{
	if (!work || !work->worker)
		return 0;
	return kthread_wait_until_idle(work->worker, work);
}

void kthread_flush_worker(struct kthread_worker *worker)
{
	if (!worker)
		return;
	kthread_wait_until_idle(worker, 0);
}

void kthread_destroy_worker(struct kthread_worker *worker)
{
	if (!worker)
		return;

	/* Drain first: stopping with items still queued would silently drop work
	 * the caller believes it submitted. */
	kthread_wait_until_idle(worker, 0);

	u64 flags;
	spin_lock_irqsave((spinlock_t *)&worker->lock_word, &flags);
	worker->stop = 1;
	spin_unlock_irqrestore((spinlock_t *)&worker->lock_word, flags);
	scheduler_wake_all(worker);

	while (!worker->exited) {
		if (!scheduler_can_block()) {
			__asm__ volatile("pause");
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare(worker);
		if (!worker->exited)
			scheduler_wait_commit();
		else
			scheduler_wait_cancel();
	}

	/* The thread has run its last instruction before setting `exited`, but it
	 * still has to be scheduled off; give it a tick before the memory it was
	 * running on goes away. */
	scheduler_sleep_ticks(1);
	lkpi_kfree(worker);
}

u64 kthread_worker_executed(struct kthread_worker *worker)
{
	return worker ? worker->executed : 0;
}
