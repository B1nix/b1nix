/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: wait queues.
 *
 * The parking logic itself lives in the wait_event macros in <lkpi/wait.h>,
 * because the predicate has to be re-evaluated inside the loop and a function
 * cannot do that. What is left here is the bookkeeping the macros call into:
 * the waiter counter, the wake, and the one place that decides what to do when
 * the caller is not allowed to sleep.
 */

#include <lkpi/env.h>
#include <lkpi/wait.h>

void init_waitqueue_head(struct wait_queue_head *wq)
{
	if (!wq)
		return;
	wq->wakeups = 0;
	wq->waiters = 0;
	lkpi_spin_lock_init(&wq->lock);
	INIT_LIST_HEAD(&wq->head);
}

void lkpi_wait_enter(struct wait_queue_head *wq)
{
	if (wq)
		__atomic_fetch_add(&wq->waiters, 1u, __ATOMIC_ACQ_REL);
}

void lkpi_wait_leave(struct wait_queue_head *wq)
{
	if (wq)
		__atomic_fetch_sub(&wq->waiters, 1u, __ATOMIC_ACQ_REL);
}

int lkpi_wait_may_block(void)
{
	return lkpi_can_block();
}

void lkpi_wait_relax(void)
{
	/* Cannot park — early boot or an interrupt handler. A driver waiting
	 * there is a bug, but spinning still makes progress, and it must keep
	 * servicing TLB shootdowns or the CPU that sent one waits forever. */
	lkpi_cpu_relax();
}

void wake_up(struct wait_queue_head *wq)
{
	if (!wq)
		return;
	/* Count the wake before releasing the waiters: a woken task that reads
	 * the counter must never see a value that predates its own wakeup. */
	__atomic_fetch_add(&wq->wakeups, 1ull, __ATOMIC_ACQ_REL);
	lkpi_wake_all(wq);
	__wake_up(wq, 0, 0, 0);
}

void wake_up_all(struct wait_queue_head *wq)
{
	wake_up(wq);
}

u64 waitqueue_wakeups(const struct wait_queue_head *wq)
{
	return wq ? __atomic_load_n(&wq->wakeups, __ATOMIC_ACQUIRE) : 0;
}

u32 waitqueue_waiters(const struct wait_queue_head *wq)
{
	return wq ? __atomic_load_n(&wq->waiters, __ATOMIC_ACQUIRE) : 0;
}

/* ── callback entries ───────────────────────────────────────────── */

/*
 * The other half of Linux's wait queue: waiters that are functions rather than
 * parked tasks. See <lkpi/wait.h> for why the shapes are upstream's.
 *
 * The walk takes a snapshot of `next` before invoking a callback, because a
 * callback is allowed to remove its own entry — i915's fence callbacks do
 * exactly that, and re-reading the link afterwards would follow a freed node.
 * The lock is dropped around the callback for the opposite reason: the callback
 * commonly signals the next fence in a chain, which takes that fence's queue
 * lock and can come back to this one.
 */
/* Same reason as the walk below: a queue embedded in a memset structure has a
 * zeroed head, and adding to it would write through NULL. Establishing the
 * empty list on first use is the only point at which it can be done, because
 * nothing else here is told when such a queue comes into existence. */
static void ensure_head(struct wait_queue_head *wq)
{
	if (!wq->head.next)
		INIT_LIST_HEAD(&wq->head);
}

void __add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	if (wq && e) {
		ensure_head(wq);
		list_add(&e->entry, &wq->head);
	}
}

void __add_wait_queue_entry_tail(struct wait_queue_head *wq,
                                 struct wait_queue_entry *e)
{
	if (wq && e) {
		ensure_head(wq);
		list_add_tail(&e->entry, &wq->head);
	}
}

void __remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	(void)wq;
	if (e)
		list_del(&e->entry);
}

void add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	if (!wq || !e)
		return;
	lkpi_spin_lock(&wq->lock);
	__add_wait_queue_entry_tail(wq, e);
	lkpi_spin_unlock(&wq->lock);
}

void remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
	if (!wq || !e)
		return;
	lkpi_spin_lock(&wq->lock);
	__remove_wait_queue(wq, e);
	lkpi_spin_unlock(&wq->lock);
}

void __wake_up(struct wait_queue_head *wq, unsigned mode, int nr, void *key)
{
	if (!wq)
		return;

	lkpi_spin_lock(&wq->lock);
	/*
	 * A queue that was never handed to init_waitqueue_head has a zeroed head,
	 * and a zeroed list head is not an empty one — an empty list points at
	 * itself. Callers embed a wait_queue_head in a structure they memset, so
	 * this is the common case rather than the exception, and walking from a
	 * NULL `next` is a fault on the first iteration. Nothing can have been
	 * added to such a queue, so there is nothing to wake.
	 */
	struct list_head *pos = wq->head.next;
	int woken = 0;

	if (!pos) {
		lkpi_spin_unlock(&wq->lock);
		return;
	}

	while (pos != &wq->head) {
		struct wait_queue_entry *e =
			container_of(pos, struct wait_queue_entry, entry);
		/* Read the successor now: the callback may unlink `e`. */
		struct list_head *next = pos->next;

		if (e->func) {
			lkpi_spin_unlock(&wq->lock);
			e->func(e, mode, 0, key);
			lkpi_spin_lock(&wq->lock);
		}
		woken++;
		if (nr && woken >= nr)
			break;
		pos = next;
	}
	lkpi_spin_unlock(&wq->lock);
}

/*
 * The wake callbacks a queued entry carries.
 *
 * b1nix wakes by channel rather than by walking entries, so by the time one of
 * these runs the waker has already published on the channel and there is
 * nothing left to do for the task. They exist because imported code walks a
 * wait queue and calls every entry's func directly — i915's sw-fence does
 * exactly that when a fence completes — so every entry that can be on such a
 * queue needs a callback that is safe to call.
 *
 * Returning 1 means "this waiter was woken", which is what a caller counts when
 * it wakes a limited number of them.
 */
int default_wake_function(struct wait_queue_entry *entry, unsigned mode,
                          int flags, void *key)
{
	(void)entry;
	(void)mode;
	(void)flags;
	(void)key;
	return 1;
}

int autoremove_wake_function(struct wait_queue_entry *entry, unsigned mode,
                             int flags, void *key)
{
	int woken = default_wake_function(entry, mode, flags, key);

	/*
	 * Unlink on wake, which is what the name promises and what makes this
	 * safe for an entry that lives on the waiter's stack: the walk that called
	 * us is holding the queue's lock and using the _safe iterator, so removing
	 * the current entry is allowed.
	 */
	if (woken)
		list_del_init(&entry->entry);
	return woken;
}
