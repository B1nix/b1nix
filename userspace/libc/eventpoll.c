/* M56 — userspace wrappers for the event-loop / IPC primitives:
 * eventfd, epoll, timerfd, signalfd. Thin shims over the matching syscalls. */

#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <syscall.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>

/* Map a negative-errno syscall return into the (-1, errno) C convention. */
static long setret(long r) {
    if (r < 0) {
        errno = (int)(-r);
        return -1;
    }
    return r;
}

/* ---- eventfd ----------------------------------------------------------- */

int eventfd(unsigned int initval, int flags) {
    return (int)setret(syscall(SYS_EVENTFD2, (long)initval, (long)flags));
}

int eventfd_read(int fd, eventfd_t *value) {
    eventfd_t v;
    ssize_t n = read(fd, &v, sizeof(v));
    if (n != (ssize_t)sizeof(v))
        return -1;
    if (value)
        *value = v;
    return 0;
}

int eventfd_write(int fd, eventfd_t value) {
    ssize_t n = write(fd, &value, sizeof(value));
    return (n == (ssize_t)sizeof(value)) ? 0 : -1;
}

/* ---- epoll ------------------------------------------------------------- */

int epoll_create1(int flags) {
    return (int)setret(syscall(SYS_EPOLL_CREATE1, (long)flags));
}

int epoll_create(int size) {
    /* The size hint is ignored since Linux 2.6.8; just validate it. */
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    return epoll_create1(0);
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    return (int)setret(
        syscall(SYS_EPOLL_CTL, (long)epfd, (long)op, (long)fd, (long)event));
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
               int timeout) {
    return (int)setret(syscall(SYS_EPOLL_WAIT, (long)epfd, (long)events,
                               (long)maxevents, (long)timeout));
}

/* ---- timerfd ----------------------------------------------------------- */

int timerfd_create(int clockid, int flags) {
    return (int)setret(syscall(SYS_TIMERFD_CREATE, (long)clockid, (long)flags));
}

int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value) {
    return (int)setret(syscall(SYS_TIMERFD_SETTIME, (long)fd, (long)flags,
                               (long)new_value, (long)old_value));
}

/* ---- signalfd ---------------------------------------------------------- */

int signalfd(int fd, const sigset_t *mask, int flags) {
    uint64_t m = mask ? (uint64_t)*mask : 0;
    return (int)setret(syscall(SYS_SIGNALFD4, (long)fd, (long)m, (long)flags));
}

/* ---- inotify (M73) ----------------------------------------------------- */

int inotify_init1(int flags) {
    return (int)setret(syscall(SYS_INOTIFY_INIT1, (long)flags));
}

int inotify_init(void) {
    return inotify_init1(0);
}

int inotify_add_watch(int fd, const char *pathname, uint32_t mask) {
    return (int)setret(
        syscall(SYS_INOTIFY_ADD_WATCH, (long)fd, (long)pathname, (long)mask));
}

int inotify_rm_watch(int fd, int wd) {
    return (int)setret(syscall(SYS_INOTIFY_RM_WATCH, (long)fd, (long)wd));
}
