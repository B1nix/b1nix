#ifndef B1NIX_RTC_H
#define B1NIX_RTC_H

#include <b1nix/types.h>

void rtc_init(void);
u32 rtc_now_unix_seconds(void);
void rtc_set_unix_time(u32 unix_time_now);

extern u32 rtc_boot_time_seconds;

#endif
