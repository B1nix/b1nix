#include <unistd.h>
#include <b1nix/syscall.h>

int write(int fd, const void *buf, size_t count)
{
	/* For our kernel, write to stdout by default */
	(void)fd;
	return (int)syscall_dispatch(SYS_WRITE, (u64)(usize)buf, (u64)count, 0, 0);
}

int read(int fd, void *buf, size_t count)
{
	return (int)syscall_dispatch(SYS_READ, (u64)fd, (u64)(usize)buf, (u64)count, 0);
}

int close(int fd)
{
	syscall_dispatch(SYS_CLOSE, (u64)fd, 0, 0, 0);
	return 0;
}

void _exit(int status)
{
	syscall_dispatch(SYS_EXIT, (u64)status, 0, 0, 0);
	while (1);
}

int sleep(unsigned int seconds)
{
	/* Assuming 100 ticks per second */
	syscall_dispatch(SYS_SLEEP, (u64)seconds * 100, 0, 0, 0);
	return 0;
}
