// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel timers that fire from the timer interrupt (M131). See
 * <b1nix/ktimer.h>.
 *
 * linuxkpi's own timers are delayed work on its system workqueue, serviced by
 * one thread in ten-millisecond steps. That suits a driver's timeout. It does
 * not suit a guest's clock: KVM injects the PIT's ticks and each LAPIC timer
 * from hrtimer callbacks, and a guest waiting on those waited on whatever else
 * that thread had queued -- a BIOS polling for its tick crawled through
 * seconds of disk reads. Here the tick itself runs them.
 *
 * The list is short (a timer per VM and per vCPU) and unsorted; the tick walks
 * it only when the earliest deadline has come.
 */
#include <b1nix/ktimer.h>
#include <b1nix/arch.h>
#include <b1nix/ktime.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>

static spinlock_t g_kt_lock;
static struct ktimer *g_kt_head;
static volatile u64 g_kt_next_ns; /* earliest armed deadline, ~0 if none */

static int kt_cpu(void)
{
	struct percpu *pc = get_percpu();

	return pc ? (int)pc->cpu_id : 0;
}

static u64 kt_ns_to_tick(u64 ns)
{
	u64 per = 1000000000ull / (sched_tick_hz() ? sched_tick_hz() : 1000);

	return (ns + per - 1) / per;
}

/* Under the lock. */
static void kt_unlink(struct ktimer *t)
{
	for (struct ktimer **pp = &g_kt_head; *pp; pp = &(*pp)->next) {
		if (*pp == t) {
			*pp = t->next;
			t->next = 0;
			t->queued = 0;
			return;
		}
	}
}

/* Under the lock. */
static void kt_recompute_next(void)
{
	u64 next = ~0ull;

	for (struct ktimer *t = g_kt_head; t; t = t->next)
		if (t->expires_ns < next)
			next = t->expires_ns;
	__atomic_store_n(&g_kt_next_ns, next, __ATOMIC_RELEASE);
}

void ktimer_arm(struct ktimer *t, u64 expires_ns)
{
	u64 flags;

	spin_lock_irqsave(&g_kt_lock, &flags);
	if (t->queued)
		kt_unlink(t);
	t->expires_ns = expires_ns;
	t->next = g_kt_head;
	g_kt_head = t;
	t->queued = 1;
	kt_recompute_next();
	spin_unlock_irqrestore(&g_kt_lock, flags);
	/* A tickless idle CPU sleeps to the scheduler's next deadline: this is
	 * one, and the scheduler kicks whoever would sleep through it. */
	sched_note_deadline(kt_ns_to_tick(expires_ns));
}

int ktimer_cancel(struct ktimer *t)
{
	int was;
	u64 flags;
	int me = kt_cpu() + 1;

	spin_lock_irqsave(&g_kt_lock, &flags);
	was = t->queued;
	if (was) {
		kt_unlink(t);
		kt_recompute_next();
	}
	spin_unlock_irqrestore(&g_kt_lock, flags);
	/* A callback on another CPU finishes before the caller may free what it
	 * reads. Its own callback cancelling it must not wait for itself. */
	while (1) {
		int r = __atomic_load_n(&t->running, __ATOMIC_ACQUIRE);

		if (!r || r == me)
			break;
		cpu_relax();
	}
	return was;
}

void ktimer_tick(void)
{
	u64 now, flags;

	if (__atomic_load_n(&g_kt_next_ns, __ATOMIC_ACQUIRE) == ~0ull ||
	    !g_kt_head)
		return;
	now = ktime_monotonic_ns();
	if (now < __atomic_load_n(&g_kt_next_ns, __ATOMIC_ACQUIRE))
		return;
	for (;;) {
		struct ktimer *due = 0;

		spin_lock_irqsave(&g_kt_lock, &flags);
		for (struct ktimer *t = g_kt_head; t; t = t->next)
			if (t->expires_ns <= now) {
				due = t;
				break;
			}
		if (due) {
			kt_unlink(due);
			/* Marked running before the lock is dropped, so a cancel
			 * that finds it unqueued still waits for it. */
			__atomic_store_n(&due->running, kt_cpu() + 1, __ATOMIC_RELEASE);
		}
		kt_recompute_next();
		spin_unlock_irqrestore(&g_kt_lock, flags);
		if (!due)
			break;
		due->fn(due->arg);
		__atomic_store_n(&due->running, 0, __ATOMIC_RELEASE);
	}
}

u64 ktimer_next_tick(void)
{
	u64 next = __atomic_load_n(&g_kt_next_ns, __ATOMIC_ACQUIRE);

	return (next == ~0ull || !g_kt_head) ? 0 : kt_ns_to_tick(next);
}
