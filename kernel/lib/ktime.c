/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/arch.h>
#include <b1nix/ktime.h>
#include <b1nix/sched.h>
#include <b1nix/vdso.h>

/* Nanoseconds per scheduler tick, at whatever rate the timer was armed with.
 * Hardcoding 10 ms here dated from a 100 Hz tick and made the pre-TSC clock
 * report ten times the elapsed time once the LAPIC took the tick to 1 kHz. */
static inline u64 ktime_ns_per_tick(void)
{
	u32 hz = sched_tick_hz();
	return hz ? 1000000000ull / hz : 10000000ull;
}

/* Handover state: the clock reads the tick counter until ktime_switch_to_tsc()
 * publishes a base, from which point it reads the TSC and adds the base so the
 * value never jumps backwards. Written once, on the BSP, before any AP can
 * observe tsc_active — hence the release/acquire pair. */
static u64 ktime_base_ns;
static u64 ktime_tsc_origin;
static volatile int ktime_tsc_active;

static u64 ktime_tick_ns(void)
{
	return scheduler_get_uptime_ticks() * ktime_ns_per_tick();
}

void ktime_switch_to_tsc(void)
{
	if (ktime_tsc_active)
		return;
	if (!arch_tsc_clock_ready())
		return;

	u64 tsc_ns = arch_tsc_monotonic_ns();
	if (tsc_ns == 0)
		return;

	ktime_base_ns = ktime_tick_ns();
	ktime_tsc_origin = tsc_ns;
	__atomic_store_n(&ktime_tsc_active, 1, __ATOMIC_RELEASE);

	/* The wall clock is built on this clock, so the vDSO needs the handover
	 * to compute CLOCK_REALTIME; before it, only the system call can. */
	u64 flags;
	struct vdso_data *d = vdso_write_begin(&flags);
	d->ktime_base_ns = ktime_base_ns;
	d->ktime_origin_ns = ktime_tsc_origin;
	d->ktime_active = 1;
	vdso_write_end(flags);
}

/* Continue the monotonic clock across a sleep that reset its source (M129).
 *
 * `counter_ns_before` is what the counter read on the way down and `slept_ns`
 * how long the machine was away, measured by the one clock that kept running —
 * the hardware clock. The counter is re-anchored to the sum, which is all it
 * takes: this kernel's clock is the counter plus a fixed base, and the clock
 * userspace reads through the vDSO IS the counter, so moving the counter's
 * anchor carries both. Nothing here adjusts the base, because a base adjustment
 * would fix the kernel's clock and leave every program's reading an interval
 * that ended before it began.
 */
void ktime_resume(u64 counter_ns_before, u64 slept_ns)
{
	if (!__atomic_load_n(&ktime_tsc_active, __ATOMIC_ACQUIRE))
		return;
	arch_tsc_reanchor(counter_ns_before + slept_ns);
}

/* CLOCK_MONOTONIC as USERSPACE reads it.
 *
 * Not the same number as ktime_monotonic_ns(): that one is the counter plus a
 * fixed base taken at the handover, while both the system call and the vDSO
 * answer the counter itself. The two therefore differ by a constant for the
 * life of the boot, and any kernel code that compares a deadline USERSPACE
 * computed has to use this one. Doing otherwise cost systemd its timers: a
 * timerfd deadline resolved against the kernel's base fired a few hundred
 * microseconds before the deadline on the caller's clock, sd-event found no
 * event source due, re-armed nothing, and PID 1 slept until something
 * unrelated woke it -- a `.timer` unit asking for one second ran fifteen
 * seconds late, and only on the boots where the constant had the wrong sign. */
u64 ktime_user_monotonic_ns(void)
{
	u64 tsc_ns = arch_tsc_monotonic_ns();

	return tsc_ns ? tsc_ns : ktime_tick_ns();
}

u64 ktime_monotonic_ns(void)
{
	if (!__atomic_load_n(&ktime_tsc_active, __ATOMIC_ACQUIRE))
		return ktime_tick_ns();

	u64 tsc_ns = arch_tsc_monotonic_ns();
	if (tsc_ns < ktime_tsc_origin)
		return ktime_base_ns;
	return ktime_base_ns + (tsc_ns - ktime_tsc_origin);
}
