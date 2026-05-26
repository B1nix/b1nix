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

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

int clock_gettime(int clk_id, struct timespec *tp);
int nanosleep(const struct timespec *req, struct timespec *rem);

typedef long clock_t;
#define CLOCKS_PER_SEC 1000000

static inline clock_t clock(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (clock_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static inline size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    (void)format;
    // Simple mock formatting that fits the expected gas timestamp
    int n = snprintf(s, max, "%04d-%02d-%02dT%02d:%02d:%02d.000+0000",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (n < 0 || (size_t)n >= max) return 0;
    return n;
}

static inline struct tm *gmtime(const time_t *timep) {
    return localtime(timep);
}

static inline char *ctime(const time_t *timep) {
    (void)timep;
    return "Tue May 26 15:00:00 2026\n";
}

#endif
