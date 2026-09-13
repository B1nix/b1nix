/*
 * The wall clock, built on the monotonic clock.
 *
 * Wall time is the monotonic reading plus a base, so it cannot move except
 * when it is told to: set (a step, which may go backwards, as settimeofday may)
 * or slewed. A slew is applied as a rate change of at most SLEW_PPM, the way
 * adjtime(3) works, so a correction of either sign never makes the clock read
 * an earlier time than it just did. Stepping the clock by a whole second to
 * "slew" it did exactly that, and wall-clock readers saw time run backwards.
 */
#include <b1nix/ktime.h>
#include <b1nix/rtc.h>
#include <b1nix/spinlock.h>

#define SLEW_PPM 500

static spinlock_t g_wall_lock = SPINLOCK_INIT;
static i64 g_base_ns;    /* wall time at monotonic zero */
static i64 g_slew_ns;    /* correction still to be applied */
static u64 g_slew_mono;  /* monotonic time the pending correction started at */
static int g_inited;

/* How much of the pending correction has been applied by `mono`. */
static i64 slew_applied(u64 mono)
{
	if (!g_slew_ns)
		return 0;
	u64 limit = (mono - g_slew_mono) / (1000000u / SLEW_PPM);
	u64 mag = g_slew_ns < 0 ? (u64)-g_slew_ns : (u64)g_slew_ns;
	if (limit > mag)
		limit = mag;
	return g_slew_ns < 0 ? -(i64)limit : (i64)limit;
}

void wallclock_init(u64 unix_seconds)
{
	u64 flags;
	spin_lock_irqsave(&g_wall_lock, &flags);
	if (!g_inited) {
		g_base_ns = (i64)(unix_seconds * 1000000000ull) - (i64)ktime_monotonic_ns();
		g_inited = 1;
	}
	spin_unlock_irqrestore(&g_wall_lock, flags);
}

int wallclock_ready(void)
{
	return g_inited;
}

u64 wallclock_now_ns(void)
{
	u64 flags;
	spin_lock_irqsave(&g_wall_lock, &flags);
	u64 mono = ktime_monotonic_ns();
	i64 ns = g_base_ns + (i64)mono + slew_applied(mono);
	spin_unlock_irqrestore(&g_wall_lock, flags);
	return ns < 0 ? 0 : (u64)ns;
}

void wallclock_set_ns(u64 unix_ns)
{
	u64 flags;
	spin_lock_irqsave(&g_wall_lock, &flags);
	g_base_ns = (i64)unix_ns - (i64)ktime_monotonic_ns();
	g_slew_ns = 0;
	g_inited = 1;
	spin_unlock_irqrestore(&g_wall_lock, flags);
}

void wallclock_slew_ns(i64 delta_ns)
{
	u64 flags;
	spin_lock_irqsave(&g_wall_lock, &flags);
	u64 mono = ktime_monotonic_ns();
	i64 done = slew_applied(mono);
	g_base_ns += done;
	g_slew_ns = g_slew_ns - done + delta_ns;
	g_slew_mono = mono;
	spin_unlock_irqrestore(&g_wall_lock, flags);
}
