#ifndef B1NIX_U_SYS_TIME_H
#define B1NIX_U_SYS_TIME_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int utimes(const char *filename, const struct timeval times[2]);
int futimes(int fd, const struct timeval times[2]);

/* BSD timeval helper macros (fontconfig fccache and others use timercmp). */
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)
#define timerclear(tvp) ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#define timercmp(a, b, CMP)                  \
  (((a)->tv_sec == (b)->tv_sec)              \
       ? ((a)->tv_usec CMP(b)->tv_usec)      \
       : ((a)->tv_sec CMP(b)->tv_sec))
#define timeradd(a, b, result)                           \
  do {                                                   \
    (result)->tv_sec = (a)->tv_sec + (b)->tv_sec;        \
    (result)->tv_usec = (a)->tv_usec + (b)->tv_usec;     \
    if ((result)->tv_usec >= 1000000) {                  \
      ++(result)->tv_sec;                                \
      (result)->tv_usec -= 1000000;                      \
    }                                                    \
  } while (0)
#define timersub(a, b, result)                           \
  do {                                                   \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;        \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec;     \
    if ((result)->tv_usec < 0) {                         \
      --(result)->tv_sec;                                \
      (result)->tv_usec += 1000000;                      \
    }                                                    \
  } while (0)

/* Interval timers. b1nix has no SIGALRM-driven interval timer yet, so
 * setitimer/getitimer are accepted no-ops (BusyBox ping uses them only to
 * schedule repeat sends; `ping -c 1` does not depend on them firing). */
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct itimerval {
  struct timeval it_interval;
  struct timeval it_value;
};

int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value);
int getitimer(int which, struct itimerval *curr_value);

#ifdef __cplusplus
}
#endif

#endif
