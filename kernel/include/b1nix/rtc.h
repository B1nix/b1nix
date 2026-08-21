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

extern u64 rtc_boot_time_seconds;

/* M107: /dev/rtc0 (and the /dev/rtc alias) — the hardware clock as a character
 * device, with the RTC_RD_TIME / RTC_SET_TIME / RTC_ALM_* / RTC_WKALM_* ioctls
 * `hwclock` and `rtcwake` drive. Implemented in kernel/dev/rtc_dev.c. */
void rtc_dev_init(void);
void rtc_dev_register_nodes(void);

#endif
