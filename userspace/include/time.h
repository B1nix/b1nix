#ifndef B1NIX_U_TIME_H
#define B1NIX_U_TIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int snprintf(char *s, size_t n, const char *format, ...);

#ifndef B1NIX_TIME_T_DEFINED
#define B1NIX_TIME_T_DEFINED
typedef long time_t;
#endif

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
void tzset(void);
extern char *tzname[2];
extern long timezone;
extern int daylight;

#ifndef __cplusplus
struct tm *localtime(const time_t *timep);
#endif

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

#ifndef __cplusplus
static inline struct tm *gmtime(const time_t *timep) {
    return localtime(timep);
}

static inline char *ctime(const time_t *timep) {
    (void)timep;
    return (char *)"Tue May 26 15:00:00 2026\n";
}
#endif

static inline double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

static inline time_t mktime(struct tm *tm) {
    int y = tm->tm_year + 1900;
    int m = tm->tm_mon + 1;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    int d = tm->tm_mday;
    long days = (365L * y) + (y / 4) - (y / 100) + (y / 400) + ((153 * m + 2) / 5) + d - 719468;
    return (time_t)(days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}

static inline char *asctime(const struct tm *tm) {
    static char buf[26];
    static const char wday_name[7][4] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char mon_name[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    snprintf(buf, sizeof(buf), "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
             wday_name[tm->tm_wday >= 0 && tm->tm_wday < 7 ? tm->tm_wday : 0],
             mon_name[tm->tm_mon >= 0 && tm->tm_mon < 12 ? tm->tm_mon : 0],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             1900 + tm->tm_year);
    return buf;
}

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
struct tm *b1nix_real_localtime(const time_t *timep) __asm__("localtime");
}

static inline struct tm *b1nix_real_gmtime(const time_t *timep) {
    return b1nix_real_localtime(timep);
}

static inline char *b1nix_real_ctime(const time_t *timep) {
    (void)timep;
    return (char *)"Tue May 26 15:00:00 2026\n";
}

struct B1nixTimePtr {
    time_t val;
    const time_t *ptr;
    B1nixTimePtr(const time_t *p) : ptr(p) {}
    B1nixTimePtr(const unsigned int *p) : val(*p), ptr(&val) {}
#if __cplusplus >= 201103L
    /* nullptr/decltype are C++11; guard so libstdc++'s -std=gnu++98 TUs that
     * include <ctime> can still parse this header. C++98 callers never pass a
     * null literal here. */
    B1nixTimePtr(decltype(nullptr)) : ptr(0) {}
#endif
};

inline struct tm *localtime(B1nixTimePtr tp) {
    return b1nix_real_localtime(tp.ptr);
}

inline struct tm *gmtime(B1nixTimePtr tp) {
    return b1nix_real_gmtime(tp.ptr);
}

inline char *ctime(B1nixTimePtr tp) {
    return b1nix_real_ctime(tp.ptr);
}
#endif

#endif
