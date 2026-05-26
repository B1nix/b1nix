#include "syscall.h"
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

int normalize_errno(long rc) {
  int e = (int)(-rc);
  if (e >= EPERM && e <= EINPROGRESS)
    return e;
  return EIO;
}

static inline int _check_err(long rc) {
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return (int)rc;
}

int write(int fd, const void *buf, size_t n) {
  return _check_err(syscall(SYS_WRITE, fd, buf, n));
}

int read(int fd, void *buf, size_t n) {
  return _check_err(syscall(SYS_READ, fd, buf, n));
}

int close(int fd) { return _check_err(syscall(SYS_CLOSE, fd)); }

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
  return _check_err(syscall(SYS_OPEN, path, flags, mode));
}

int unlink(const char *pathname) {
  return _check_err(syscall(SYS_UNLINK, pathname));
}

int rmdir(const char *pathname) {
  return _check_err(syscall(SYS_RMDIR, pathname));
}

int mprotect(void *addr, size_t len, int prot) {
  return _check_err(syscall(SYS_MPROTECT, addr, len, prot));
}

long lseek(int fd, long offset, int whence) {
  long rc = syscall(SYS_LSEEK, fd, offset, whence);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return rc;
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
  return _check_err(syscall(SYS_EXECVE, pathname, argv, envp));
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
  return _check_err(syscall(SYS_MKDIR, path, mode));
}

int chdir(const char *path) {
  return _check_err(syscall(SYS_CHDIR, path));
}

char *getcwd(char *buf, size_t size) {
  if (!buf) {
    size_t alloc_size = size == 0 ? 1024 : size;
    buf = (char *)malloc(alloc_size);
    if (!buf) {
      errno = normalize_errno(-ENOMEM);
      return NULL;
    }
    size = alloc_size;
  }
  long rc = syscall(SYS_GETCWD, buf, size);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return NULL;
  }
  return buf;
}

int fsync(int fd) { return _check_err(syscall(SYS_FSYNC, fd)); }

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
  return _check_err(syscall(SYS_STAT, path, st));
}

int statfs(const char *path, struct statfs *buf) {
  return _check_err(syscall(SYS_STATFS, path, buf));
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd,
           long offset) {
  long rc = syscall(SYS_MMAP, addr, length, prot, flags, fd, offset);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return (void *)-1;
  }
  return (void *)rc;
}

int munmap(void *addr, size_t length) {
  return _check_err(syscall(SYS_MUNMAP, addr, length));
}

int socket(int domain, int type, int protocol) {
  return _check_err(syscall(SYS_SOCKET, domain, type, protocol));
}

int bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
  return _check_err(syscall(SYS_BIND, fd, addr, addrlen));
}

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
  return _check_err(syscall(SYS_CONNECT, fd, addr, addrlen));
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
  long rc = syscall(SYS_SEND, fd, buf, len, flags);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return (ssize_t)rc;
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
  long rc = syscall(SYS_RECV, fd, buf, len, flags);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return (ssize_t)rc;
}

int listen(int fd, int backlog) {
  return _check_err(syscall(SYS_LISTEN, fd, backlog));
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
  return _check_err(syscall(SYS_ACCEPT, fd, addr, addrlen));
}

int setuid(unsigned short uid) {
  return _check_err(syscall(SYS_SETUID, uid));
}

int setgid(unsigned short gid) {
  return _check_err(syscall(SYS_SETGID, gid));
}

int clock_gettime(int clk_id, struct timespec *tp) {
  return _check_err(syscall(SYS_CLOCK_GETTIME, clk_id, tp));
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
  if (!req) return -1;
  unsigned long ticks = req->tv_sec * 100 + req->tv_nsec / 10000000;
  if (ticks == 0 && req->tv_nsec > 0) {
    ticks = 1;
  }
  long rc = syscall(SYS_SLEEP, ticks);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  if (rem) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

unsigned int alarm(unsigned int seconds) {
  (void)seconds;
  return 0;
}

int fchmod(int fd, mode_t mode) {
  (void)fd;
  (void)mode;
  return 0;
}

int utime(const char *filename, const void *times) {
  (void)filename;
  (void)times;
  return 0;
}

int fork(void) {
  return _check_err(syscall(SYS_FORK));
}

int pipe(int pipefd[2]) {
  return _check_err(syscall(SYS_PIPE, pipefd));
}

int dup2(int oldfd, int newfd) {
  return _check_err(syscall(SYS_DUP2, oldfd, newfd));
}

int execv(const char *pathname, char *const argv[]) {
  extern char **environ;
  return _check_err(syscall(SYS_EXECVE, pathname, argv, environ));
}

int kill(int pid, int sig) {
  return _check_err(syscall(SYS_KILL, pid, sig));
}

pid_t wait(int *wstatus) {
  return _check_err(syscall(SYS_WAIT, wstatus));
}

pid_t waitpid(pid_t pid, int *wstatus, int options) {
  return _check_err(syscall(SYS_WAITPID, pid, wstatus, options));
}
