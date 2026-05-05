#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <fcntl.h>
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
	return syscall(SYS_SLEEP, seconds, 0, 0, 0);
}

int open(const char *path, int flags, ...)
{
	unsigned int mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, unsigned int);
		va_end(ap);
	}
	return syscall(SYS_OPEN, (long)path, flags, mode, 0);
}

int unlink(const char *pathname)
{
	return syscall(SYS_UNLINK, (long)pathname, 0, 0, 0);
}

int mprotect(void *addr, size_t len, int prot)
{
	(void)addr; (void)len; (void)prot;
	return 0; // Stub
}

long lseek(int fd, long offset, int whence)
{
	return syscall(SYS_LSEEK, fd, offset, whence, 0);
}

int execvp(const char *file, char *const argv[])
{
	(void)file; (void)argv;
	return -1; // Stub
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
	return syscall(SYS_GETCWD, (long)buf, size, 0, 0);
}

time_t time(time_t *tloc)
{
	time_t t = syscall(SYS_TIME, 0, 0, 0, 0);
	if (tloc) *tloc = t;
	return t;
}

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
	(void)tz;
	if (tv) {
		tv->tv_sec = syscall(SYS_TIME, 0, 0, 0, 0);
		tv->tv_usec = 0;
	}
	return 0;
}

struct tm *localtime(const time_t *timep)
{
	static struct tm t;
	(void)timep;
	// Minimal stub
	t.tm_sec = 0; t.tm_min = 0; t.tm_hour = 0;
	t.tm_mday = 1; t.tm_mon = 0; t.tm_year = 70;
	t.tm_wday = 0; t.tm_yday = 0; t.tm_isdst = 0;
	return &t;
}



int stat(const char *path, struct stat *st)
{
	return (int)syscall(SYS_STAT, (long)path, (long)st, 0, 0);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
	(void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
	return (void *)syscall(SYS_MMAP, length, 0, 0, 0);
}

int munmap(void *addr, size_t length)
{
	(void)length;
	return syscall(SYS_MUNMAP, (long)addr, 0, 0, 0);
}
