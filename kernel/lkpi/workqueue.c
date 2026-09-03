/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M99 linuxkpi: workqueues over kthread_create.
 * See kernel/include/lkpi/workqueue.h.
 */

#include <b1nix/arch.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <lkpi/workqueue.h>
#include <string.h>

struct workqueue_struct {
	char name[24];
	struct work_struct *head;
	struct work_struct *tail;
	struct delayed_work *delayed;
	spinlock_t lock;
	volatile int stop;
	volatile int running;   /* thread is executing a handler */
	volatile int alive;     /* thread has not exited */
	volatile u64 processed; /* completed items, for flush_workqueue */
	volatile u64 queued;    /* accepted items */
};

void INIT_WORK(struct work_struct *work, work_func_t func)
{
	if (!work)
		return;
	memset(work, 0, sizeof(*work));
	work->func = func;
}

void INIT_DELAYED_WORK(struct delayed_work *dwork, work_func_t func)
{
	if (!dwork)
		return;
	memset(dwork, 0, sizeof(*dwork));
	dwork->work.func = func;
}

/* Pop the head of the FIFO. Caller must NOT hold the lock. */
static struct work_struct *wq_dequeue(struct workqueue_struct *wq)
{
	u64 flags;
	spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
	struct work_struct *w = wq->head;
	if (w) {
		wq->head = w->next;
		if (!wq->head)
			wq->tail = 0;
		w->next = 0;
		w->pending = 0;
		w->wq = 0;
		w->running = 1;
	}
	spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
	return w;
}

/* A queue with nothing delayed still wakes this often, so a wake that went
 * missing costs a fraction of a second rather than the machine. */
#define WQ_IDLE_MAX_TICKS 20

/* Move every delayed item whose deadline has passed onto the run queue. */
static void wq_arm_due(struct workqueue_struct *wq)
{
	u64 now = scheduler_get_ticks();
	for (;;) {
		u64 flags;
		spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
		struct delayed_work **pp = &wq->delayed;
		struct delayed_work *found = 0;
		while (*pp) {
			if ((*pp)->due_tick <= now) {
				found = *pp;
				*pp = found->next;
				found->next = 0;
				found->armed = 0;
				break;
			}
			pp = &(*pp)->next;
		}
		if (!found) {
			spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
			return;
		}
		struct work_struct *w = &found->work;
		if (!w->pending) {
			w->pending = 1;
			w->wq = wq;
			w->next = 0;
			if (wq->tail)
				wq->tail->next = w;
			else
				wq->head = w;
			wq->tail = w;
			wq->queued++;
		}
		spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
	}
}

/* How long this thread may sleep with nothing to run.
 *
 * It used to be one tick, unconditionally, which made this thread a second
 * 100 Hz heartbeat: measured on the aarch64 sys lane, once net_task stopped
 * waking the machine every tick, lkpi-events took over and ended 1,832 idle
 * stretches of exactly one tick each. A timer that fires only to discover it
 * has nothing to do is the thing dynamic ticks exist to remove.
 *
 * So sleep until the earliest delayed item is actually due. Queued work does
 * not come through here at all -- queue_work wakes this thread -- so the
 * timeout is only ever about the delayed list. Capped, because a queue with no
 * delayed work at all should still come back occasionally rather than park for
 * ever on a wake it might miss. */
static u64 wq_idle_timeout(struct workqueue_struct *wq)
{
	u64 now = scheduler_get_ticks();
	u64 best = 0;
	u64 flags;

	spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
	for (struct delayed_work *d = wq->delayed; d; d = d->next) {
		u64 wait = d->due_tick > now ? d->due_tick - now : 1;

		if (!best || wait < best)
			best = wait;
	}
	spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);

	if (!best || best > WQ_IDLE_MAX_TICKS)
		best = WQ_IDLE_MAX_TICKS;
	return best;
}

static void workqueue_thread(void *arg)
{
	struct workqueue_struct *wq = arg;
	wq->alive = 1;
	while (!wq->stop) {
		wq_arm_due(wq);
		struct work_struct *w = wq_dequeue(wq);
		if (!w) {
			/* Nothing to run. Park on the queue itself; queue_work wakes
			 * it, and the one-tick timeout brings it back to arm whatever
			 * delayed item has come due in the meantime.
			 *
			 * A pending delayed item is NOT a reason to cancel the wait. It
			 * used to be, and since wq_arm_due had already found nothing due,
			 * the cancel put the thread straight back into a loop with no
			 * sleep in it at all: arm nothing, dequeue nothing, prepare,
			 * cancel, again. One delayed work with a deadline in the future
			 * was enough to make this thread spin on the boot CPU until that
			 * deadline arrived -- and on this arch userspace runs only on the
			 * boot CPU, so everything else on the machine stopped. It showed
			 * up as ordinary commands taking nine seconds, and, when the
			 * deadline was far enough out, as the watchdog calling the
			 * instance deadlocked. The timeout already services delayed
			 * items; sleeping through it is the point. */
			scheduler_wait_prepare_timeout(wq, wq_idle_timeout(wq));
			if (wq->head || wq->stop)
				scheduler_wait_cancel();
			else
				scheduler_wait_commit();
			continue;
		}
		if (w->func)
			w->func(w);
		u64 flags;
		spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
		w->running = 0;
		w->seq++;
		wq->processed++;
		spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
		/* Wake anyone in flush_work/flush_workqueue. */
		scheduler_wake_all(w);
		scheduler_wake_all((void *)(usize)&wq->processed);
	}
	wq->alive = 0;
	scheduler_wake_all((void *)(usize)&wq->processed);
	scheduler_exit_current(0);
}

struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags,
                                         int max_active)
{
	(void)flags;
	(void)max_active; /* see the note in <lkpi/workqueue.h> */
	struct workqueue_struct *wq = kzalloc(sizeof(*wq));
	if (!wq)
		return 0;
	wq->lock = SPINLOCK_INIT;
	usize n = name ? strlen(name) : 0;
	if (n > sizeof(wq->name) - 1)
		n = sizeof(wq->name) - 1;
	if (n)
		memcpy(wq->name, name, n);
	wq->name[n] = '\0';

	if (kthread_create(wq->name[0] ? wq->name : "lkpi-wq", workqueue_thread,
	                   wq) < 0) {
		kfree(wq);
		return 0;
	}
	return wq;
}

int queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	if (!wq || !work || !work->func)
		return 0;
	u64 flags;
	spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
	if (work->pending) {
		spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
		return 0;
	}
	work->pending = 1;
	work->wq = wq;
	work->next = 0;
	if (wq->tail)
		wq->tail->next = work;
	else
		wq->head = work;
	wq->tail = work;
	wq->queued++;
	spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
	scheduler_wake_all(wq);
	return 1;
}

int queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
                       u64 delay_ticks)
{
	if (!wq || !dwork || !dwork->work.func)
		return 0;
	if (delay_ticks == 0)
		return queue_work(wq, &dwork->work);

	u64 flags;
	spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
	if (dwork->armed || dwork->work.pending) {
		spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
		return 0;
	}
	dwork->due_tick = scheduler_get_ticks() + delay_ticks;
	dwork->wq = wq;
	dwork->armed = 1;
	dwork->next = wq->delayed;
	wq->delayed = dwork;
	spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
	scheduler_wake_all(wq);
	return 1;
}

int cancel_delayed_work(struct delayed_work *dwork)
{
	if (!dwork || !dwork->wq)
		return 0;
	struct workqueue_struct *wq = dwork->wq;
	u64 flags;
	int removed = 0;
	spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
	struct delayed_work **pp = &wq->delayed;
	while (*pp) {
		if (*pp == dwork) {
			*pp = dwork->next;
			dwork->next = 0;
			dwork->armed = 0;
			removed = 1;
			break;
		}
		pp = &(*pp)->next;
	}
	spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
	return removed;
}

int flush_work(struct work_struct *work)
{
	if (!work)
		return 0;
	int waited = 0;
	while (work->pending || work->running) {
		waited = 1;
		if (!scheduler_can_block()) {
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare_timeout(work, 1);
		if (!work->pending && !work->running)
			scheduler_wait_cancel();
		else
			scheduler_wait_commit();
	}
	return waited;
}

void flush_workqueue(struct workqueue_struct *wq)
{
	if (!wq)
		return;
	for (;;) {
		u64 flags;
		spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
		int idle = (wq->head == 0) && (wq->running == 0) &&
		           (wq->processed >= wq->queued);
		spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
		if (idle)
			return;
		if (!scheduler_can_block()) {
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare_timeout((void *)(usize)&wq->processed, 1);
		scheduler_wait_commit();
	}
}

void destroy_workqueue(struct workqueue_struct *wq)
{
	if (!wq)
		return;
	flush_workqueue(wq);
	wq->stop = 1;
	scheduler_wake_all(wq);
	/* Wait for the thread to observe the stop flag before freeing the struct
	 * it is still reading. */
	for (int i = 0; i < 1000 && wq->alive; i++) {
		if (!scheduler_can_block())
			break;
		scheduler_sleep_ticks(1);
	}
	if (!wq->alive)
		kfree(wq);
	/* If the thread never parked (no scheduler), the struct is deliberately
	 * leaked rather than freed under a live reader. */
}

static struct workqueue_struct *g_system_wq;
static struct workqueue_struct *g_system_unbound_wq;

struct workqueue_struct *lkpi_system_wq(void)
{
	if (!g_system_wq)
		g_system_wq = alloc_workqueue("lkpi-events", 0, 1);
	return g_system_wq;
}

/*
 * A pool of its own, because upstream's two names are two pools.
 *
 * Each queue here is served by exactly one thread, so a work item that queues
 * more work and then waits for it deadlocks if both land in the same queue —
 * nothing is left to run the inner item. That is not hypothetical: RMFB runs
 * drm_mode_rmfb_work_fn on system_wq, which removes the framebuffer from its
 * planes through an atomic commit, and that commit queues commit_work on
 * system_unbound_wq and waits. Aliasing the two names wedged the only worker
 * and, behind it, every later commit — a page flip then never sent its
 * completion event and userspace waited on a frame that could not land.
 *
 * Linux does not hit this because system_wq is a per-CPU pool and
 * system_unbound_wq an unbound one with real concurrency; keeping them
 * separate is the property the imported code is entitled to, not a workaround.
 */
struct workqueue_struct *lkpi_system_unbound_wq(void)
{
	if (!g_system_unbound_wq)
		g_system_unbound_wq = alloc_workqueue("lkpi-unbound", 0, 1);
	return g_system_unbound_wq;
}

int workqueue_pending(struct workqueue_struct *wq)
{
	if (!wq)
		return 0;
	/* Delayed items count: one that has not fired yet is work this queue still
	 * owes, and a drain that ignored them would return with the queue about to
	 * become busy again. */
	return wq->head != 0 || wq->delayed != 0 || wq->running != 0;
}
