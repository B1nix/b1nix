#ifndef _SYS_EPOLL_H
#define _SYS_EPOLL_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* epoll event masks (Linux ABI; reuse the poll bits). */
#define EPOLLIN      0x001
#define EPOLLPRI     0x002
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLRDNORM  0x040
#define EPOLLRDBAND  0x080
#define EPOLLWRNORM  0x100
#define EPOLLWRBAND  0x200
#define EPOLLRDHUP   0x2000
#define EPOLLONESHOT (1u << 30)
#define EPOLLET      (1u << 31)

/* epoll_create1 flags. */
#define EPOLL_CLOEXEC 0x00080000

/* epoll_ctl ops. */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
    void     *ptr;
    int       fd;
    uint32_t  u32;
    uint64_t  u64;
} epoll_data_t;

/* Packed to match the Linux x86_64 ABI (12 bytes). */
struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
} __attribute__((packed));

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_EPOLL_H */
