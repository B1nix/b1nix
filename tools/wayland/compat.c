#include <unistd.h>

#include "compat.h"

#undef read
#undef write

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);

/* eventfd / epoll / timerfd / signalfd are now real libc primitives (M56) — the
 * port no longer emulates them. With a real (single-fd) eventfd, the old
 * read/write redirection is unnecessary, so these are plain passthroughs (kept
 * only because the server is compiled with read/write remapped). */
ssize_t b1nix_wayland_read(int fd, void *buf, size_t count)
{
	return read(fd, buf, count);
}

ssize_t b1nix_wayland_write(int fd, const void *buf, size_t count)
{
	return write(fd, buf, count);
}
