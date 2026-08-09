/*
 * SPDX-License-Identifier: MIT
 *
 * M101 linuxkpi: one-shot timers.
 *
 * Built on lkpi's delayed work, which already has a thread servicing deadlines,
 * so a timer costs no extra thread. The consequence is stated in the header and
 * repeated here because it changes what a callback may do: the callback runs in
 * process context and MAY sleep, where Linux runs it in softirq context and it
 * may not. That is a relaxation, never a restriction, so imported callbacks
 * stay correct — but code written against this must not assume the reverse.
 */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <lkpi/env.h>
#include <lkpi/workqueue.h>

static void timer_work_fn(struct work_struct *work)
{
	struct delayed_work *dwork = (struct delayed_work *)work;
	struct timer_list *timer = (struct timer_list *)dwork;
	if (timer->function)
		timer->function(timer);
}

void timer_setup(struct timer_list *timer, void (*func)(struct timer_list *),
                 unsigned int flags)
{
	(void)flags;
	if (!timer)
		return;
	INIT_DELAYED_WORK(&timer->dwork, timer_work_fn);
	timer->function = func;
	timer->expires = 0;
	timer->initialised = 1;
}

int mod_timer(struct timer_list *timer, unsigned long expires_jiffies)
{
	if (!timer || !timer->initialised)
		return -EINVAL;

	/* Linux's expires is an absolute deadline; the queue takes a delay. A
	 * deadline already past becomes a delay of zero rather than a huge one,
	 * which is what the unsigned subtraction would otherwise produce. */
	u64 now = lkpi_ticks();
	u64 delay = (expires_jiffies > now) ? (u64)expires_jiffies - now : 0;

	int was_armed = cancel_delayed_work(&timer->dwork);
	timer->expires = expires_jiffies;
	queue_delayed_work(lkpi_system_wq(), &timer->dwork, delay);
	return was_armed;
}

int del_timer_sync(struct timer_list *timer)
{
	if (!timer || !timer->initialised)
		return 0;
	int was_armed = cancel_delayed_work(&timer->dwork);
	/* Sync: the caller is about to free the object the callback reads, so it
	 * must not return while the callback is still running. */
	flush_work(&timer->dwork.work);
	return was_armed;
}
