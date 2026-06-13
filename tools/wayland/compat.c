#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "compat.h"

#undef read
#undef write

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);

#define MAX_EPOLL 8
#define MAX_WATCH 64
#define MAX_EVENTFD 8

struct watch {
	int used;
	int fd;
	struct epoll_event event;
};
struct epoll_set {
	int used;
	int id;
	struct watch watch[MAX_WATCH];
};
struct event_pipe {
	int used;
	int read_fd;
	int write_fd;
};

static struct epoll_set sets[MAX_EPOLL];
static struct event_pipe event_pipes[MAX_EVENTFD];
static int next_epoll_id = 0x4000;

static struct epoll_set *find_set(int id)
{
	for (int i = 0; i < MAX_EPOLL; i++)
		if (sets[i].used && sets[i].id == id)
			return &sets[i];
	return NULL;
}

int epoll_create1(int flags)
{
	(void)flags;
	for (int i = 0; i < MAX_EPOLL; i++)
		if (!sets[i].used) {
			memset(&sets[i], 0, sizeof(sets[i]));
			sets[i].used = 1;
			sets[i].id = next_epoll_id++;
			return sets[i].id;
		}
	errno = EMFILE;
	return -1;
}

int epoll_create(int size)
{
	(void)size;
	return epoll_create1(0);
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	struct epoll_set *set = find_set(epfd);
	struct watch *free_watch = NULL;
	if (!set) {
		errno = EBADF;
		return -1;
	}
	for (int i = 0; i < MAX_WATCH; i++) {
		struct watch *watch = &set->watch[i];
		if (!watch->used) {
			if (!free_watch)
				free_watch = watch;
			continue;
		}
		if (watch->fd != fd)
			continue;
		if (op == EPOLL_CTL_DEL)
			memset(watch, 0, sizeof(*watch));
		else if (op == EPOLL_CTL_MOD && event)
			watch->event = *event;
		else {
			errno = EEXIST;
			return -1;
		}
		return 0;
	}
	if (op != EPOLL_CTL_ADD || !event || !free_watch) {
		errno = EINVAL;
		return -1;
	}
	free_watch->used = 1;
	free_watch->fd = fd;
	free_watch->event = *event;
	return 0;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
	struct epoll_set *set = find_set(epfd);
	struct pollfd fds[MAX_WATCH];
	struct watch *watches[MAX_WATCH];
	int count = 0;
	if (!set) {
		errno = EBADF;
		return -1;
	}
	for (int i = 0; i < MAX_WATCH; i++)
		if (set->watch[i].used) {
			fds[count].fd = set->watch[i].fd;
			fds[count].events = 0;
			if (set->watch[i].event.events & EPOLLIN)
				fds[count].events |= POLLIN;
			if (set->watch[i].event.events & EPOLLOUT)
				fds[count].events |= POLLOUT;
			fds[count].revents = 0;
			watches[count++] = &set->watch[i];
		}
	int ready = poll(fds, count, timeout);
	if (ready <= 0)
		return ready;
	int out = 0;
	for (int i = 0; i < count && out < maxevents; i++) {
		if (!fds[i].revents)
			continue;
		events[out] = watches[i]->event;
		events[out].events = 0;
		if (fds[i].revents & POLLIN) events[out].events |= EPOLLIN;
		if (fds[i].revents & POLLOUT) events[out].events |= EPOLLOUT;
		if (fds[i].revents & POLLERR) events[out].events |= EPOLLERR;
		if (fds[i].revents & POLLHUP) events[out].events |= EPOLLHUP;
		out++;
	}
	return out;
}

int eventfd(unsigned int initval, int flags)
{
	int fds[2];
	(void)flags;
	if (pipe(fds) < 0)
		return -1;
	for (int i = 0; i < MAX_EVENTFD; i++)
		if (!event_pipes[i].used) {
			event_pipes[i].used = 1;
			event_pipes[i].read_fd = fds[0];
			event_pipes[i].write_fd = fds[1];
			if (initval) {
				uint64_t value = initval;
				write(fds[1], &value, sizeof(value));
			}
			return fds[0];
		}
	close(fds[0]);
	close(fds[1]);
	errno = EMFILE;
	return -1;
}

ssize_t b1nix_wayland_read(int fd, void *buf, size_t count)
{
	return read(fd, buf, count);
}

ssize_t b1nix_wayland_write(int fd, const void *buf, size_t count)
{
	for (int i = 0; i < MAX_EVENTFD; i++)
		if (event_pipes[i].used && event_pipes[i].read_fd == fd)
			return write(event_pipes[i].write_fd, buf, count);
	return write(fd, buf, count);
}

int timerfd_create(int clockid, int flags)
{
	(void)clockid;
	return eventfd(0, flags);
}

int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
		    struct itimerspec *old_value)
{
	(void)fd;
	(void)flags;
	(void)new_value;
	(void)old_value;
	return 0;
}

int signalfd(int fd, const sigset_t *mask, int flags)
{
	(void)fd;
	(void)mask;
	return eventfd(0, flags);
}
