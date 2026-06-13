#ifndef B1NIX_WAYLAND_COMPAT_H
#define B1NIX_WAYLAND_COMPAT_H

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

FILE *b1nix_wayland_open_memstream(char **buffer, size_t *size);
#define open_memstream b1nix_wayland_open_memstream

#ifdef B1NIX_WAYLAND_SERVER
#define EPOLLIN 0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLL_CLOEXEC 1
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EFD_CLOEXEC 1
#define EFD_NONBLOCK 2
#define TFD_CLOEXEC 1
#define TFD_NONBLOCK 2
#define TFD_TIMER_ABSTIME 1
#define SFD_CLOEXEC 1
#define SFD_NONBLOCK 2
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
};

typedef uint64_t eventfd_t;
struct itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};
struct signalfd_siginfo {
	uint32_t ssi_signo;
	uint8_t pad[124];
};

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
int eventfd(unsigned int initval, int flags);
int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
		    struct itimerspec *old_value);
int signalfd(int fd, const sigset_t *mask, int flags);
ssize_t b1nix_wayland_read(int fd, void *buf, size_t count);
ssize_t b1nix_wayland_write(int fd, const void *buf, size_t count);

static inline int flock(int fd, int operation)
{
	struct flock lock = {
		.l_type = operation & LOCK_UN ? F_UNLCK :
			  operation & LOCK_EX ? F_WRLCK : F_RDLCK,
		.l_whence = SEEK_SET,
	};
	return fcntl(fd, operation & LOCK_NB ? F_SETLK : F_SETLKW, &lock);
}

#define read b1nix_wayland_read
#define write b1nix_wayland_write
#endif

#endif
