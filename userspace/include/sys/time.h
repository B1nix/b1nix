#ifndef B1NIX_U_SYS_TIME_H
#define B1NIX_U_SYS_TIME_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int utimes(const char *filename, const struct timeval times[2]);

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
