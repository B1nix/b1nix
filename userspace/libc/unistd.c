#include <unistd.h>
#include "syscall.h"

int write(int fd, const void *buf, size_t n)
{
	return (int)syscall(SYS_WRITE, (long)buf, (long)n, (long)fd, 1);
}

int read(int fd, void *buf, size_t n)
{
	return (int)syscall(SYS_READ, (long)fd, (long)buf, (long)n, 0);
}

int close(int fd)
{
	return (int)syscall(SYS_CLOSE, (long)fd, 0, 0, 0);
}

void _exit(int status)
{
	syscall(SYS_EXIT, (long)status, 0, 0, 0);
	while (1);
}

int sleep(unsigned int seconds)
{
	return (int)syscall(SYS_SLEEP, (long)seconds * 100, 0, 0, 0);
}

int open(const char *path, int flags)
{
	return (int)syscall(SYS_OPEN, (long)path, (long)flags, 0, 0);
}

int mkdir(const char *path, unsigned int mode)
{
	return (int)syscall(SYS_MKDIR, (long)path, (long)mode, 0, 0);
}

int chdir(const char *path)
{
	return (int)syscall(SYS_CHDIR, (long)path, 0, 0, 0);
}

int getcwd(char *buf, size_t size)
{
	return (int)syscall(SYS_GETCWD, (long)buf, (long)size, 0, 0);
}

long time(void)
{
	return syscall(SYS_TIME, 0, 0, 0, 0);
}

int stat(const char *path, struct stat *st)
{
	return (int)syscall(SYS_STAT, (long)path, (long)st, 0, 0);
}
