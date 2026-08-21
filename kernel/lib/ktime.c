#include <b1nix/arch.h>
#include <b1nix/ktime.h>
#include <b1nix/sched.h>

/* Nanoseconds per scheduler tick (the scheduler runs at 100 Hz). */
#define KTIME_NS_PER_TICK 10000000ull

/* Handover state: the clock reads the tick counter until ktime_switch_to_tsc()
 * publishes a base, from which point it reads the TSC and adds the base so the
 * value never jumps backwards. Written once, on the BSP, before any AP can
 * observe tsc_active — hence the release/acquire pair. */
static u64 ktime_base_ns;
static u64 ktime_tsc_origin;
static volatile int ktime_tsc_active;

static u64 ktime_tick_ns(void)
{
	return scheduler_get_uptime_ticks() * KTIME_NS_PER_TICK;
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
