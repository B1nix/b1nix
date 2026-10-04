/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TIME_H
#define LKPI_LINUX_TIME_H
#include <linux/ktime.h>
#include <linux/time64.h>

/* A broken-down time, in the kernel's form: tm_year counts from 1900 and is a
 * long, so years past 2^31 do not wrap. */
struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	long tm_year;
	int tm_wday;
	int tm_yday;
};

void time64_to_tm(time64_t totalsecs, int offset, struct tm *result);
/* The inverse: a calendar date and time (month 1..12, full year) as seconds
 * since the epoch. ISO 9660 stores its timestamps this way. */
time64_t mktime64(const unsigned int year, const unsigned int mon,
                  const unsigned int day, const unsigned int hour,
                  const unsigned int min, const unsigned int sec);

/* The machine's timezone as settimeofday last set it. FAT stores local time,
 * and converts through this. */
struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};
extern struct timezone sys_tz;



#endif
