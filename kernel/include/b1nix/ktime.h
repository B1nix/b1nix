#ifndef B1NIX_KTIME_H
#define B1NIX_KTIME_H

#include <b1nix/types.h>

/* The kernel's single monotonic clock.
 *
 * Everything that reports "time since boot" — the dmesg timestamps, the
 * /dev/kmsg record stamps and /proc/uptime — reads this one function, so an
 * operator comparing a boot log against /proc/uptime sees the same numbers.
 *
 * Before the LAPIC calibrates the TSC the only tick source is the 100 Hz
 * scheduler counter, so early boot resolves to 10 ms steps; once
 * ktime_switch_to_tsc() runs the clock continues from that value with
 * nanosecond resolution. It never goes backwards across the handover. */
u64 ktime_monotonic_ns(void);

/* Hand the clock over to the calibrated TSC. Called once, after lapic_init(). */
void ktime_switch_to_tsc(void);

#endif
