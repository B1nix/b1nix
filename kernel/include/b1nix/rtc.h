/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_RTC_H
#define B1NIX_RTC_H

#include <b1nix/types.h>

void rtc_init(void);
u64 rtc_now_unix_seconds(void);
/* The same clock in nanoseconds. Every reader that wants a sub-second wall
 * time must take BOTH halves from here: composing seconds from one clock and
 * nanoseconds from another produces a time that goes backwards. */
u64 rtc_now_unix_nanos(void);
void rtc_set_unix_time(u64 unix_time_now);

/* The wall clock itself (kernel/lib/wallclock.c): monotonic time plus a base.
 * Set steps it; slew corrects it at a bounded rate and never moves it back. */
void wallclock_init(u64 unix_seconds);
int wallclock_ready(void);
u64 wallclock_now_ns(void);
void wallclock_set_ns(u64 unix_ns);
void wallclock_slew_ns(i64 delta_ns);

/* Seconds since the epoch for a civil (proleptic Gregorian) date, exact through
 * the 100/400-year leap rules. Exposed so the conversion can be tested against
 * known answers rather than trusted; see rtc_selftest(). */
u64 rtc_civil_to_unix(u16 year, u32 month, u32 day, u32 hour, u32 minute,
                      u32 second);

/* Known-answer test for the above; emits M118-RTC markers. Test mode only. */
void rtc_selftest(void);

extern u64 rtc_boot_time_seconds;

/* M107: /dev/rtc0 (and the /dev/rtc alias) — the hardware clock as a character
 * device, with the RTC_RD_TIME / RTC_SET_TIME / RTC_ALM_* / RTC_WKALM_* ioctls
 * `hwclock` and `rtcwake` drive. Implemented in kernel/dev/rtc_dev.c. */
void rtc_dev_init(void);
void rtc_dev_register_nodes(void);

/* M129: the alarm as a wake source. Claims the interrupt the alarm raises
 * (IRQ 8 on x86_64, the PL031's GIC line on aarch64), acknowledges it at the
 * device and registers the RTC with the suspend path, so an alarm armed
 * through RTC_WKALM_SET can end an s2idle. Called once at boot. */
void rtc_wake_source_init(void);
/* How many alarm interrupts this machine has taken. /proc/interrupts. */
u64 rtc_wake_irq_count(void);

#endif
