/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M99 linuxkpi: workqueues over kthread_create.
 * See kernel/include/lkpi/workqueue.h.
 */

#include <b1nix/arch.h>
#include <b1nix/errno.h>
#include <b1nix/klog.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <lkpi/workqueue.h>
#include <stdarg.h>
#include <stdio.h>
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
	/*
	 * How many worker threads this queue has, and how many of them are
	 * currently inside a handler.
	 *
	 * One thread per queue is not enough for a filesystem. A work item is
	 * allowed to BLOCK — btrfs's metadata read completion waits for another
	 * block, and that block's completion is queued to the same queue — so a
	 * single worker deadlocks against itself: the item that would unblock it
	 * is behind it in its own queue. It looked exactly like a lost wakeup.
	 *
	 * Upstream solves this by starting another worker when one blocks, which
	 * is what `max_active` bounds. This does the same, on demand: queue_work
	 * starts a thread when every existing one is busy and there is work
	 * waiting.
	 */
	volatile int workers;   /* threads created */
	volatile int busy;      /* threads inside a handler */
	int max_workers;
	struct workqueue_struct *reg_next;  /* the registry below */
};

/* The ceiling on workers per queue.
 *
 * Upstream's default is one per CPU for a bound queue and 256 for an unbound
 * one. This is far lower because each is a full kernel thread with its own
 * stack, and the queues here are deep rather than wide: what matters is that a
 * blocked item cannot stop the queue, not that many run at once. */
#define LKPI_WQ_MAX_WORKERS 8

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

/*
 * The items a worker thread is inside right now, by address.
 *
 * A handler may free the item it was called with -- btrfs's bio completion
 * frees the btrfs_bio that embeds its work_struct -- so nothing here may touch
 * the item once its handler has been called. Clearing a `running` field in it
 * afterwards wrote into freed memory, and the page cache entry that had taken
 * the block by then had its LRU link overwritten. flush_work asks this table
 * instead, as upstream's worker pool compares current_work by address.
 */
#define WQ_RUNNING_SLOTS 256
static struct work_struct *g_wq_running[WQ_RUNNING_SLOTS];
static spinlock_t g_wq_running_lock = SPINLOCK_INIT;

static int wq_running_enter(struct work_struct *w)
{
	u64 flags;
	int slot = -1;

	spin_lock_irqsave(&g_wq_running_lock, &flags);
	for (int i = 0; i < WQ_RUNNING_SLOTS; i++)
		if (!g_wq_running[i]) {
			g_wq_running[i] = w;
			slot = i;
			break;
		}
	spin_unlock_irqrestore(&g_wq_running_lock, flags);
	return slot;
}

static void wq_running_leave(int slot)
{
	u64 flags;

	if (slot < 0)
		return;
	spin_lock_irqsave(&g_wq_running_lock, &flags);
	g_wq_running[slot] = 0;
	spin_unlock_irqrestore(&g_wq_running_lock, flags);
}

static int wq_is_running(const struct work_struct *w)
{
	u64 flags;
	int found = 0;

	spin_lock_irqsave(&g_wq_running_lock, &flags);
	for (int i = 0; i < WQ_RUNNING_SLOTS && !found; i++)
		found = (g_wq_running[i] == w);
	spin_unlock_irqrestore(&g_wq_running_lock, flags);
	return found;
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
		/* Before the handler: see g_wq_running. */
		w->seq++;
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
		int slot = wq_running_enter(w);
		if (w->func) {
			u64 bflags;

			spin_lock_irqsave((spinlock_t *)&wq->lock, &bflags);
			wq->busy++;
			spin_unlock_irqrestore((spinlock_t *)&wq->lock, bflags);

			w->func(w);

			spin_lock_irqsave((spinlock_t *)&wq->lock, &bflags);
			wq->busy--;
			spin_unlock_irqrestore((spinlock_t *)&wq->lock, bflags);
		}
		/* `w` may be freed from here on: only its address is used. */
		wq_running_leave(slot);
		u64 flags;
		spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
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

/*
 * Every queue, so a stuck unmount can be asked what its queues are doing.
 * flush_workqueue() waits for one to drain and says nothing about why it has
 * not; this is what turns that into a name and a count.
 */
static struct workqueue_struct *wq_registry;
static spinlock_t wq_registry_lock = SPINLOCK_INIT;

/*
 * The variadic form the linux header routes to.
 *
 * Upstream's alloc_workqueue takes a FORMAT string, and btrfs names most of
 * its queues "btrfs-%s" with the subsystem as the argument. Dropping the
 * arguments left every one of them called "btrfs-%s", which makes a queue dump
 * useless for telling them apart.
 */
struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags,
                                         int max_active);

struct workqueue_struct *lkpi_alloc_workqueue(const char *fmt,
                                              unsigned int flags,
                                              int max_active, ...)
{
	char name[24];
	va_list ap;

	va_start(ap, max_active);
	vsnprintf(name, sizeof(name), fmt ? fmt : "lkpi-wq", ap);
	va_end(ap);
	return alloc_workqueue(name, flags, max_active);
}

void lkpi_wq_dump(void)
{
	struct workqueue_struct *wq;
	u64 flags;

	spin_lock_irqsave(&wq_registry_lock, &flags);
	for (wq = wq_registry; wq; wq = wq->reg_next) {
		console_write("lkpi-wq ");
		console_write(wq->name[0] ? wq->name : "(unnamed)");
		console_write(" queued=");
		console_write_dec(wq->queued);
		console_write(" done=");
		console_write_dec(wq->processed);
		console_write(" pending=");
		console_write_dec(wq->head ? 1 : 0);
		console_write(" workers=");
		console_write_dec((u64)wq->workers);
		console_write(" busy=");
		console_write_dec((u64)wq->busy);
		console_write(" chan=0x");
		console_write_hex64((u64)(usize)&wq->processed);
		console_write("\n");
	}
	spin_unlock_irqrestore(&wq_registry_lock, flags);
}

struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags,
                                         int max_active)
{
	(void)flags;
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

	wq->max_workers = max_active > 0 && max_active < LKPI_WQ_MAX_WORKERS
	                      ? max_active
	                      : LKPI_WQ_MAX_WORKERS;

	if (kthread_create(wq->name[0] ? wq->name : "lkpi-wq", workqueue_thread,
	                   wq) < 0) {
		kfree(wq);
		return 0;
	}
	wq->workers = 1;
	{
		u64 rflags;

		spin_lock_irqsave(&wq_registry_lock, &rflags);
		wq->reg_next = wq_registry;
		wq_registry = wq;
		spin_unlock_irqrestore(&wq_registry_lock, rflags);
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
	int need_worker = (wq->busy >= wq->workers && wq->workers < wq->max_workers);

	if (need_worker)
		wq->workers++;   /* claimed under the lock, created below */
	spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);

	/*
	 * Start another worker when every existing one is inside a handler.
	 *
	 * A work item may block — btrfs's metadata completion waits for a read
	 * whose own completion is queued here — and with one thread the queue
	 * then waits for itself. The check is "all busy", not "queue non-empty",
	 * so a queue whose items never block never grows past one thread.
	 *
	 * If the thread cannot be created the count is put back: the item still
	 * runs, just behind whatever is ahead of it.
	 */
	if (need_worker &&
	    kthread_create(wq->name[0] ? wq->name : "lkpi-wq", workqueue_thread,
	                   wq) < 0) {
		spin_lock_irqsave((spinlock_t *)&wq->lock, &flags);
		wq->workers--;
		spin_unlock_irqrestore((spinlock_t *)&wq->lock, flags);
	}

	scheduler_wake_all(wq);
	return 1;
}

int queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
                       u64 delay_jiffies)
{
	u64 delay_ticks;

	if (!wq || !dwork || !dwork->work.func)
		return 0;
	if (delay_jiffies == 0)
		return queue_work(wq, &dwork->work);

	/* The caller counts in jiffies, this queue counts in scheduler ticks, and
	 * they are not the same unit: <linux/jiffies.h> fixes HZ at 100 for the
	 * imported tree while the tick runs at 1 kHz. Taking one for the other
	 * fired every delayed work ten times too early -- a console retry meant to
	 * span six seconds finished in seven hundred milliseconds, and every
	 * timeout in the imported drivers was short by the same factor. */
	{
		u32 hz = sched_tick_hz();
		u32 per_jiffy = hz / 100u;

		if (per_jiffy < 1u)
			per_jiffy = 1u;
		delay_ticks = delay_jiffies * per_jiffy;
	}

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
	while (work->pending || wq_is_running(work)) {
		waited = 1;
		if (!scheduler_can_block()) {
			cpu_relax();
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare_timeout(work, 1);
		if (!work->pending && !wq_is_running(work))
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
	{
		struct workqueue_struct **pp;
		u64 rflags;

		spin_lock_irqsave(&wq_registry_lock, &rflags);
		for (pp = &wq_registry; *pp; pp = &(*pp)->reg_next)
			if (*pp == wq) {
				*pp = wq->reg_next;
				break;
			}
		spin_unlock_irqrestore(&wq_registry_lock, rflags);
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
