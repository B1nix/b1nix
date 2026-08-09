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
