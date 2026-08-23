/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M99 linuxkpi: completions. See kernel/include/lkpi/completion.h.
 */

#include <b1nix/arch.h>
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
			cpu_relax();
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

/* Cycles the TSC advances in one scheduler tick, or 0 when the CPU clock was
 * never calibrated. The TSC runs whether or not interrupts are enabled, which
 * is the whole point of using it here. */
static u64 tsc_per_tick(void)
{
#if defined(__aarch64__)
	/* CNTVCT_EL0 advances at CNTFRQ_EL0, not at the CPU clock, so the CPU's
	 * calibrated kHz would be the wrong scale here. A tick is 10 ms. */
	u64 hz;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
	return hz ? hz / 100ull : 0;
#else
	u32 khz = arch_cpu_khz();

	/* A tick is 10 ms, so a tick is 10 * (cycles per ms). */
	return khz ? (u64)khz * 10ull : 0;
#endif
}

/* A cycle counter that keeps running with interrupts off. x86_64 has the TSC;
 * aarch64's equivalent is the virtual counter CNTVCT_EL0, which is likewise
 * independent of the timer interrupt. */
static inline u64 read_tsc(void)
{
#if defined(__x86_64__)
	u32 lo, hi;

	__asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
#elif defined(__aarch64__)
	u64 v;

	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
#else
	return 0;
#endif
}

u64 wait_for_completion_timeout(struct completion *c, u64 timeout_ticks)
{
	if (!c)
		return 0;
	u64 start = scheduler_get_ticks();
	u64 deadline = start + timeout_ticks;
	/*
	 * A second deadline on the CPU's own clock.
	 *
	 * The tick deadline is only reachable while the timer interrupt can run.
	 * A caller that cannot block is a caller with interrupts disabled — that
	 * is exactly what scheduler_can_block() reports — and the tick then never
	 * advances, so a timeout expressed in ticks can never expire and the spin
	 * below runs forever. That is not hypothetical: i915's atomic commit waits
	 * for a page flip this way, and the machine stopped there.
	 *
	 * The TSC keeps counting regardless, so it is what bounds the spin.
	 */
	u64 tsc_deadline = 0;
	u64 per_tick = tsc_per_tick();

	if (per_tick)
		tsc_deadline = read_tsc() + timeout_ticks * per_tick;

	for (;;) {
		if (try_wait_for_completion(c)) {
			u64 now = scheduler_get_ticks();
			return now < deadline ? (deadline - now) : 1;
		}
		u64 now = scheduler_get_ticks();
		if (now >= deadline)
			return 0;
		if (!scheduler_can_block()) {
			/* Uncalibrated CPU clock leaves nothing to bound this with, so
			 * the tick deadline is all there is — see above for when that is
			 * not enough. */
			if (tsc_deadline && read_tsc() >= tsc_deadline)
				return 0;
			cpu_relax();
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
