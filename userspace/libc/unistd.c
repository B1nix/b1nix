#include "syscall.h"
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int write(int fd, const void *buf, size_t n) {
  return (int)syscall(SYS_WRITE, fd, buf, n);
}

int read(int fd, void *buf, size_t n) {
  return (int)syscall(SYS_READ, fd, buf, n);
}

int close(int fd) { return (int)syscall(SYS_CLOSE, fd); }

void _exit(int status) {
  syscall(SYS_EXIT, status);
  while (1)
    ;
}

int sleep(unsigned int seconds) {
  return (int)syscall(SYS_SLEEP, seconds);
}

int open(const char *path, int flags, ...) {
  unsigned int mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, unsigned int);
    va_end(ap);
  }
  return (int)syscall(SYS_OPEN, path, flags, mode);
}

int unlink(const char *pathname) {
  return (int)syscall(SYS_UNLINK, pathname);
}

int mprotect(void *addr, size_t len, int prot) {
  return (int)syscall(SYS_MPROTECT, addr, len, prot);
}

long lseek(int fd, long offset, int whence) {
  return (long)syscall(SYS_LSEEK, fd, offset, whence);
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
  return (int)syscall(SYS_EXECVE, pathname, argv, envp);
}

extern char **environ;

int execvp(const char *file, char *const argv[]) {
  if (file && strchr(file, '/')) {
    return execve(file, argv, environ);
  }

  // Simple PATH resolution
  char *path_env = getenv("PATH");
  if (!path_env) path_env = "/bin:/usr/bin";

  char path_buf[512];
  const char *p = path_env;
  while (*p) {
    const char *start = p;
    while (*p && *p != ':') p++;

    int len = p - start;
    if (len + 1 + strlen(file) < sizeof(path_buf)) {
      memcpy(path_buf, start, len);
      path_buf[len] = '/';
      strcpy(path_buf + len + 1, file);

      // Check if executable exists
      if (open(path_buf, O_RDONLY) >= 0) {
        return execve(path_buf, argv, environ);
      }
    }

    if (*p == ':') p++;
  }

  return execve(file, argv, environ); // Fallback to let kernel return ENOENT
}

int mkdir(const char *path, unsigned int mode) {
  return (int)syscall(SYS_MKDIR, path, mode);
}

int chdir(const char *path) {
  return (int)syscall(SYS_CHDIR, path);
}

int getcwd(char *buf, size_t size) {
  return (int)syscall(SYS_GETCWD, buf, size);
}

int fsync(int fd) { return (int)syscall(SYS_FSYNC, fd); }

void sync(void) { syscall(SYS_SYNC); }

time_t time(time_t *tloc) {
  time_t t = (time_t)syscall(SYS_TIME);
  if (tloc)
    *tloc = t;
  return t;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
  (void)tz;
  if (tv) {
    tv->tv_sec = (time_t)syscall(SYS_TIME);
    tv->tv_usec = 0;
  }
  return 0;
}

struct tm *localtime(const time_t *timep) {
  static struct tm t;
  time_t seconds = *timep;
  int days = seconds / 86400;
  int rem = seconds % 86400;

  t.tm_hour = rem / 3600;
  rem %= 3600;
  t.tm_min = rem / 60;
  t.tm_sec = rem % 60;

  t.tm_wday = (4 + days) % 7; // 1 Jan 1970 was a Thursday (4)

  int year = 1970;
  while (1) {
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    int days_in_year = leap ? 366 : 365;
    if (days < days_in_year) {
      break;
    }
    days -= days_in_year;
    year++;
  }

  t.tm_year = year - 1900;
  t.tm_yday = days;

  int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  int days_in_month[] = {31, 28 + leap, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int month = 0;
  while (days >= days_in_month[month]) {
    days -= days_in_month[month];
    month++;
  }

  t.tm_mon = month;
  t.tm_mday = days + 1;
  t.tm_isdst = 0;

  return &t;
}

int stat(const char *path, struct stat *st) {
  return (int)syscall(SYS_STAT, path, st);
}

int statfs(const char *path, struct statfs *buf) {
  return (int)syscall(SYS_STATFS, path, buf);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd,
           long offset) {
  return (void *)syscall(SYS_MMAP, addr, length, prot, flags, fd, offset);
}

int munmap(void *addr, size_t length) {
  return (int)syscall(SYS_MUNMAP, addr, length);
}

int socket(int domain, int type, int protocol) {
  return (int)syscall(SYS_SOCKET, domain, type, protocol);
}

int bind(int fd, const void *addr, size_t addrlen) {
  return (int)syscall(SYS_BIND, fd, addr, addrlen);
}

int connect(int fd, const void *addr, size_t addrlen) {
  return (int)syscall(SYS_CONNECT, fd, addr, addrlen);
}

long send(int fd, const void *buf, size_t len, int flags) {
  return (long)syscall(SYS_SEND, fd, buf, len, flags);
}

long recv(int fd, void *buf, size_t len, int flags) {
  return (long)syscall(SYS_RECV, fd, buf, len, flags);
}
