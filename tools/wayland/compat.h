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
/* eventfd / epoll / timerfd / signalfd are now real libc primitives (M56);
 * take their constants, types, and declarations from the system headers
 * instead of the old port-local emulation. */
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

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
