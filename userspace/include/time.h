#ifndef B1NIX_U_TIME_H
#define B1NIX_U_TIME_H

typedef long time_t;

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t *tloc);
int gettimeofday(struct timeval *tv, struct timezone *tz);
struct tm *localtime(const time_t *timep);

#endif
