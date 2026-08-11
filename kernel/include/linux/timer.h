/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_TIMER_H
#define LKPI_LINUX_TIMER_H
/* seqcount_t reaches i915 transitively upstream, through the time headers
 * rather than by an include of its own. Same neighbourhood here, so imported
 * code that names the type without asking for it still finds it. */
#include <linux/seqlock.h>

#include <b1nix/types.h>
#include <lkpi/workqueue.h>

/*
 * One-shot timers.
 *
 * Built on lkpi's delayed work, which already has a queue thread servicing
 * deadlines, so a timer costs no extra thread. The consequence is that the
 * callback runs in process context and MAY sleep — where Linux runs it in
 * softirq context and it may not. That is a relaxation, never a restriction, so
 * imported callbacks stay correct; but it is stated because a driver author
 * reading this must not conclude the reverse.
 */
struct timer_list {
	struct delayed_work dwork;
	void (*function)(struct timer_list *);
	unsigned long expires;
	u32 initialised;
};

void timer_setup(struct timer_list *timer, void (*func)(struct timer_list *),
                 unsigned int flags);
int mod_timer(struct timer_list *timer, unsigned long expires_jiffies);
int del_timer_sync(struct timer_list *timer);
#define timer_delete_sync(t) del_timer_sync(t)
#define add_timer(t)         mod_timer((t), (t)->expires)
/* Recover the containing object from the timer the callback was handed. */
#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, __typeof__(*var), timer_fieldname)

#define timer_pending(t)     ((t)->dwork.armed != 0)


/* The pre-shutdown spelling of timer teardown, and the shutdown form that also
 * makes re-arming fail afterwards. b1nix's timers stop when deleted and hold no
 * "shut down" state, so a re-arm after this would succeed here where upstream
 * would refuse it — every caller here deletes on the teardown path and does not
 * re-arm. */
int timer_delete(struct timer_list *timer);
#define del_timer(t)           timer_delete(t)
#define timer_shutdown_sync(t) timer_delete_sync(t)

/* Run this timer's callback with interrupts disabled. b1nix runs every timer
 * callback from the tick with interrupts already disabled, so the flag asks for
 * what it would get anyway. */
#define TIMER_IRQSAFE 0x00200000

#endif
