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

#endif
