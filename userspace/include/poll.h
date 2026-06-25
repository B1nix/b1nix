#ifndef _POLL_H
#define _POLL_H 1

#include <signal.h>
#include <time.h>

#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020

#define POLLRDNORM  0x040
#define POLLRDBAND  0x080
#define POLLWRNORM  0x100
#define POLLWRBAND  0x200
#define POLLRDHUP   0x2000  /* peer half-close; matches EPOLLRDHUP */

typedef unsigned int nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#ifdef __cplusplus
extern "C" {
#endif

int poll(struct pollfd *fds, nfds_t nfds, int timeout);
int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif /* _POLL_H */
