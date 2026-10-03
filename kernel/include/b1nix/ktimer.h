/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_KTIMER_H
#define B1NIX_KTIMER_H
#include <b1nix/types.h>

/*
 * One-shot kernel timers whose callbacks run in the timer interrupt (M131).
 *
 * Every CPU's tick looks at the armed list, so the resolution is one tick of
 * the scheduler clock, and an idle CPU's tickless sleep ends in time for the
 * earliest one. A callback runs with interrupts off and must not sleep: it is
 * what Linux calls a hard hrtimer, and KVM's guest timers (the PIT, each
 * vCPU's LAPIC timer) are the users. A callback may re-arm its own timer.
 *
 * The struct is owned by the caller and must stay put while armed; linuxkpi
 * embeds it in struct hrtimer (as struct lkpi_ktimer, the same layout).
 */
struct ktimer {
	struct ktimer *next;
	u64 expires_ns;            /* ktime_monotonic_ns() deadline */
	void (*fn)(void *arg);
	void *arg;
	volatile int queued;
	volatile int running;      /* cpu id + 1 while the callback runs */
};

/* (Re)arm for `expires_ns`; a timer already armed moves to the new time. */
void ktimer_arm(struct ktimer *t, u64 expires_ns);
/* Disarm. Waits for a callback running on another CPU; returns 1 if the timer
 * was armed. Called from the timer's own callback, it only disarms. */
int ktimer_cancel(struct ktimer *t);
/* From every CPU's timer interrupt. */
void ktimer_tick(void);
/* The scheduler tick at which the earliest armed timer is due, 0 if none. */
u64 ktimer_next_tick(void);
#endif
