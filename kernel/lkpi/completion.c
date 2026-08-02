/*
 * SPDX-License-Identifier: MIT
 *
 * M99 linuxkpi: completions. See kernel/include/lkpi/completion.h.
 */

#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <lkpi/completion.h>

/* One global guard for the (tiny) critical sections that adjust `done`. A
 * per-completion lock would double the struct for no measurable benefit — the
 * sections are three instructions long and completions are not a contended
 * resource in a GPU driver. */
static spinlock_t completion_guard = SPINLOCK_INIT;

void init_completion(struct completion *c)
{
	if (!c)
		return;
	c->done = 0;
	c->all = 0;
}

void reinit_completion(struct completion *c)
{
	if (!c)
		return;
	u64 flags;
	spin_lock_irqsave(&completion_guard, &flags);
	c->done = 0;
	c->all = 0;
	spin_unlock_irqrestore(&completion_guard, flags);
}

int completion_done(struct completion *c)
{
	if (!c)
		return 1;
	return c->all || c->done > 0;
}

int try_wait_for_completion(struct completion *c)
{
	if (!c)
		return 0;
	u64 flags;
	int got = 0;
	spin_lock_irqsave(&completion_guard, &flags);
	if (c->all) {
		got = 1;
	} else if (c->done) {
		c->done--;
		got = 1;
	}
	spin_unlock_irqrestore(&completion_guard, flags);
	return got;
}

void wait_for_completion(struct completion *c)
{
	if (!c)
		return;
	for (;;) {
		if (try_wait_for_completion(c))
			return;
		if (!scheduler_can_block()) {
			/* No scheduler yet (early boot) or interrupts are off: poll. The
			 * completion can still be set by a device interrupt in the first
			 * case and by another CPU in the second. */
			__asm__ volatile("pause");
			tlb_shootdown_poll();
			continue;
		}
		/* Two-phase wait closes the lost-wakeup window: register on the
		 * channel, re-check, then park. */
		scheduler_wait_prepare(c);
		if (completion_done(c)) {
			scheduler_wait_cancel();
			continue;
		}
		scheduler_wait_commit();
	}
}

u64 wait_for_completion_timeout(struct completion *c, u64 timeout_ticks)
{
	if (!c)
		return 0;
	u64 start = scheduler_get_ticks();
	u64 deadline = start + timeout_ticks;

	for (;;) {
		if (try_wait_for_completion(c)) {
			u64 now = scheduler_get_ticks();
			return now < deadline ? (deadline - now) : 1;
		}
		u64 now = scheduler_get_ticks();
		if (now >= deadline)
			return 0;
		if (!scheduler_can_block()) {
			__asm__ volatile("pause");
			tlb_shootdown_poll();
			continue;
		}
		scheduler_wait_prepare_timeout(c, deadline - now);
		if (completion_done(c)) {
			scheduler_wait_cancel();
			continue;
		}
		scheduler_wait_commit();
	}
}

void complete(struct completion *c)
{
	if (!c)
		return;
	u64 flags;
	spin_lock_irqsave(&completion_guard, &flags);
	c->done++;
	spin_unlock_irqrestore(&completion_guard, flags);
	scheduler_wake_all(c);
}

void complete_all(struct completion *c)
{
	if (!c)
		return;
	u64 flags;
	spin_lock_irqsave(&completion_guard, &flags);
	c->all = 1;
	spin_unlock_irqrestore(&completion_guard, flags);
	scheduler_wake_all(c);
}
