/* SPDX-License-Identifier: GPL-2.0-only */
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

/* Continue the monotonic clock across a sleep that stopped its source: what the
 * COUNTER read on the way down, plus however long the machine was away (the
 * hardware clock is what knows). Called from the resume path before anything
 * reads the clock. See kernel/lib/ktime.c. */
void ktime_resume(u64 counter_ns_before, u64 slept_ns);

/* Hand the clock over to the calibrated TSC. Called once, after lapic_init(). */
void ktime_switch_to_tsc(void);

#endif
