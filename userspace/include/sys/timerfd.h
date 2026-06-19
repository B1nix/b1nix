#ifndef _SYS_TIMERFD_H
#define _SYS_TIMERFD_H 1

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* timerfd_create flags. */
#define TFD_CLOEXEC   0x00080000
#define TFD_NONBLOCK  0x00000800

/* timerfd_settime flags. */
#define TFD_TIMER_ABSTIME 0x00000001

#ifndef __itimerspec_defined
#define __itimerspec_defined 1
struct itimerspec {
    struct timespec it_interval; /* timer period */
    struct timespec it_value;    /* timer expiration */
};
#endif

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIMERFD_H */
