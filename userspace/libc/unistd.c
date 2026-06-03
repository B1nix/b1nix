#include "syscall.h"
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <termios.h>
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

ssize_t write(int fd, const void *buf, size_t n) {
  long rc = syscall(SYS_WRITE, fd, buf, n);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return (ssize_t)rc;
}

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
  (void)flags;
  long rc = syscall(SYS_GETRANDOM, buf, buflen, 0);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return (ssize_t)rc;
}

ssize_t read(int fd, void *buf, size_t n) {
  long rc = syscall(SYS_READ, fd, buf, n);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return (ssize_t)rc;
}

int close(int fd) { return _check_err(syscall(SYS_CLOSE, fd)); }

void _exit(int status) {
  syscall(SYS_EXIT, status);
  while (1)
    ;
}

int sleep(unsigned int seconds) {
  return (int)syscall(SYS_SLEEP, (unsigned long)seconds * 100);
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

int link(const char *oldpath, const char *newpath) {
  return _check_err(syscall(SYS_LINK, oldpath, newpath));
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

int fcntl(int fd, int cmd, ...) {
  long arg = 0;
  va_list ap;
  va_start(ap, cmd);
  arg = va_arg(ap, long);
  va_end(ap);
  return _check_err(syscall(SYS_FCNTL, fd, cmd, arg));
}

int ioctl(int fd, unsigned long request, ...) {
  void *arg = 0;
  va_list ap;
  va_start(ap, request);
  arg = va_arg(ap, void *);
  va_end(ap);
  return _check_err(syscall(SYS_IOCTL, fd, request, arg));
}

time_t time(time_t *tloc) {
  /* time_t is 64-bit (long long) on every b1nix ABI, including the 32-bit
   * x86 port. The SYS_TIME syscall return value travels through a single
   * machine register (EAX on i386), so on 32-bit it would truncate the epoch
   * seconds to 32 bits — re-introducing the year-2038 wrap. Read the full
   * 64-bit tv_sec out of clock_gettime() (copied through a struct, not a
   * register) so wall-clock time stays full-width on both architectures. */
  struct timespec ts;
  time_t t;
  if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
    t = ts.tv_sec;
  else
    t = (time_t)(long)syscall(SYS_TIME); /* fallback: may truncate on i386 */
  if (tloc)
    *tloc = t;
  return t;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
  if (tv) {
    /* Prefer the higher-resolution monotonic clock so tv_usec carries real
     * sub-second detail (tick granularity, ~10 ms) instead of always 0. Fall
     * back to whole-second SYS_TIME if the clock syscall is unavailable. */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
      return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
  }
  if (tz) {
    tzset();
    tz->tz_minuteswest = (int)(timezone / 60);
    tz->tz_dsttime = daylight;
  }
  return 0;
}

extern int __b1nix_tz_dst_rule; /* from stdlib.c */

static int is_leap_year(int y) {
  return (y % 4 == 0) && ((y % 100) != 0 || (y % 400) == 0);
}

static int days_in_month(int y, int m) {
  static const int d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (m == 2) return d[m - 1] + (is_leap_year(y) ? 1 : 0);
  return d[m - 1];
}

static int day_of_week_ymd(int y, int m, int d) {
  if (m < 3) { m += 12; y -= 1; }
  int k = y % 100;
  int j = y / 100;
  int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  return (h + 6) % 7; /* 0=Sun..6=Sat */
}

static int nth_weekday_of_month(int y, int m, int weekday, int nth) {
  int d = 1;
  int w = day_of_week_ymd(y, m, d);
  int delta = (weekday - w + 7) % 7;
  d += delta + (nth - 1) * 7;
  return d;
}

static int last_weekday_of_month(int y, int m, int weekday) {
  int d = days_in_month(y, m);
  int w = day_of_week_ymd(y, m, d);
  int delta = (w - weekday + 7) % 7;
  return d - delta;
}

static long long days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int)doe - 719468;
}

static time_t epoch_from_ymdhms_utc(int y, int mon, int mday, int h, int min, int sec) {
  long long days = days_from_civil(y, (unsigned)mon, (unsigned)mday);
  long long t = days * 86400LL + h * 3600LL + min * 60LL + sec;
  return (time_t)t;
}

static void seconds_to_tm(time_t seconds, struct tm *t) {
  long long s = (long long)seconds;
  long long days = s / 86400;
  long long rem = s % 86400;
  if (rem < 0) {
    rem += 86400;
    days -= 1;
  }

  t->tm_hour = (int)(rem / 3600);
  rem %= 3600;
  t->tm_min = (int)(rem / 60);
  t->tm_sec = (int)(rem % 60);

  int wday = (int)((4 + days) % 7); /* 1970-01-01 = Thursday */
  if (wday < 0) wday += 7;
  t->tm_wday = wday;

  long long z = days + 719468;
  long long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = (int)yoe + (int)(era * 400);
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);

  t->tm_year = y - 1900;
  t->tm_mon = (int)m - 1;
  t->tm_mday = (int)d;
  t->tm_yday = (int)doy;
}

static int is_dst_active(time_t utc, int std_offset_east_sec) {
  struct tm *gt = gmtime(&utc);
  if (!gt) return 0;
  int y = gt->tm_year + 1900;

  if (__b1nix_tz_dst_rule == 1) {
    int start_day = last_weekday_of_month(y, 3, 0);  /* Mar, Sun */
    int end_day = last_weekday_of_month(y, 10, 0);   /* Oct, Sun */
    time_t start = epoch_from_ymdhms_utc(y, 3, start_day, 1, 0, 0);
    time_t end = epoch_from_ymdhms_utc(y, 10, end_day, 1, 0, 0);
    return utc >= start && utc < end;
  }

  if (__b1nix_tz_dst_rule == 2) {
    int start_day = nth_weekday_of_month(y, 3, 0, 2);  /* second Sun Mar */
    int end_day = nth_weekday_of_month(y, 11, 0, 1);   /* first Sun Nov */
    time_t start_local_std = epoch_from_ymdhms_utc(y, 3, start_day, 2, 0, 0);
    time_t end_local_dst = epoch_from_ymdhms_utc(y, 11, end_day, 2, 0, 0);
    time_t start_utc = start_local_std - (time_t)std_offset_east_sec;
    time_t end_utc = end_local_dst - (time_t)(std_offset_east_sec + 3600);
    return utc >= start_utc && utc < end_utc;
  }

  return 0;
}

struct tm *gmtime(const time_t *timep) {
  static struct tm t;
  if (!timep) return NULL;
  seconds_to_tm(*timep, &t);
  t.tm_isdst = 0;
  return &t;
}

struct tm *localtime(const time_t *timep) {
  static struct tm t;
  if (!timep) return NULL;
  tzset();
  int std_offset_east = (int)(-timezone);
  int dst_on = daylight ? is_dst_active(*timep, std_offset_east) : 0;
  time_t local = *timep + (time_t)std_offset_east + (dst_on ? 3600 : 0);
  seconds_to_tm(local, &t);
  t.tm_isdst = dst_on ? 1 : 0;

  return &t;
}

char *ctime(const time_t *timep) { return asctime(localtime(timep)); }

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
  static const char *wday_short[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  static const char *wday_long[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  static const char *mon_short[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  static const char *mon_long[12] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
  size_t out = 0;
  if (!s || max == 0 || !format || !tm) return 0;
  tzset();

  for (const char *p = format; *p; p++) {
    char buf[64];
    const char *add = NULL;
    buf[0] = '\0';
    if (*p != '%') {
      if (out + 1 >= max) return 0;
      s[out++] = *p;
      continue;
    }
    p++;
    if (!*p) break;
    switch (*p) {
      case '%': add = "%"; break;
      case 'Y': snprintf(buf, sizeof(buf), "%04d", tm->tm_year + 1900); add = buf; break;
      case 'm': snprintf(buf, sizeof(buf), "%02d", tm->tm_mon + 1); add = buf; break;
      case 'd': snprintf(buf, sizeof(buf), "%02d", tm->tm_mday); add = buf; break;
      case 'H': snprintf(buf, sizeof(buf), "%02d", tm->tm_hour); add = buf; break;
      case 'M': snprintf(buf, sizeof(buf), "%02d", tm->tm_min); add = buf; break;
      case 'S': snprintf(buf, sizeof(buf), "%02d", tm->tm_sec); add = buf; break;
      case 'a': add = wday_short[(tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0]; break;
      case 'A': add = wday_long[(tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0]; break;
      case 'b': add = mon_short[(tm->tm_mon >= 0 && tm->tm_mon < 12) ? tm->tm_mon : 0]; break;
      case 'B': add = mon_long[(tm->tm_mon >= 0 && tm->tm_mon < 12) ? tm->tm_mon : 0]; break;
      case 'z': {
        int east = (int)(-timezone) + (tm->tm_isdst ? 3600 : 0);
        int sign = east >= 0 ? '+' : '-';
        int a = east >= 0 ? east : -east;
        snprintf(buf, sizeof(buf), "%c%02d%02d", sign, a / 3600, (a % 3600) / 60);
        add = buf;
        break;
      }
      case 'Z': add = tzname[tm->tm_isdst ? 1 : 0]; break;
      case 'F': snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); add = buf; break;
      case 'T': snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); add = buf; break;
      default:
        if (out + 2 >= max) return 0;
        s[out++] = '%';
        s[out++] = *p;
        continue;
    }
    if (!add) add = "";
    size_t n = strlen(add);
    if (out + n >= max) return 0;
    memcpy(s + out, add, n);
    out += n;
  }
  s[out] = '\0';
  return out;
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

/* M32: select() — fd-readiness multiplex. Converts tv to ms (with NULL ⇒
 * wait forever, matching the b1nix poll convention). */
#include <sys/select.h>
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) {
  unsigned long ms;
  if (!timeout) {
    ms = (unsigned long)-1;
  } else {
    long total = timeout->tv_sec * 1000L + timeout->tv_usec / 1000L;
    if (total < 0) total = 0;
    ms = (unsigned long)total;
  }
  return _check_err(syscall(SYS_SELECT, nfds, readfds, writefds, exceptfds, ms));
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

int setsockopt(int sockfd, int level, int optname, const void *optval,
               socklen_t optlen) {
  return _check_err(syscall(SYS_SETSOCKOPT, sockfd, level, optname, optval,
                            (size_t)optlen));
}

int getsockopt(int sockfd, int level, int optname, void *optval,
               socklen_t *optlen) {
  if (!optlen) {
    errno = EINVAL;
    return -1;
  }
  /* The kernel reads/writes the length as a 64-bit usize; bridge via size_t. */
  size_t klen = *optlen;
  long rc = syscall(SYS_GETSOCKOPT, sockfd, level, optname, optval, &klen);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  *optlen = (socklen_t)klen;
  return 0;
}

int listen(int fd, int backlog) {
  return _check_err(syscall(SYS_LISTEN, fd, backlog));
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
  (void)dest_addr;
  (void)addrlen;
  return send(fd, buf, len, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
  (void)src_addr;
  (void)addrlen;
  return recv(fd, buf, len, flags);
}

int shutdown(int sockfd, int how) {
  return _check_err(syscall(SYS_SHUTDOWN, sockfd, how));
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
  return _check_err(syscall(SYS_ACCEPT, fd, addr, addrlen));
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  if (!addrlen) {
    errno = EINVAL;
    return -1;
  }
  size_t klen = *addrlen;
  long rc = syscall(SYS_GETSOCKNAME, sockfd, addr, &klen);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  *addrlen = (socklen_t)klen;
  return 0;
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  if (!addrlen) {
    errno = EINVAL;
    return -1;
  }
  size_t klen = *addrlen;
  long rc = syscall(SYS_GETPEERNAME, sockfd, addr, &klen);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  *addrlen = (socklen_t)klen;
  return 0;
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
  return _check_err(syscall(SYS_FCHMOD, fd, mode));
}

int ftruncate(int fd, off_t length) {
  (void)fd;
  return length == 0 ? 0 : -1;
}

#include <utime.h>
int utime(const char *filename, const struct utimbuf *times) {
  /* POSIX: a NULL times argument sets both atime and mtime to the current
   * time. Pass the two timestamps as scalar args (seconds) so the kernel
   * never has to copy a struct from user memory. */
  long atime, mtime;
  if (times) {
    atime = (long)times->actime;
    mtime = (long)times->modtime;
  } else {
    atime = mtime = (long)syscall(SYS_TIME);
  }
  return _check_err(syscall(SYS_UTIME, filename, atime, mtime));
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
  /* POSIX wait(status) == waitpid(-1, status, 0): reap any child. Route via
   * SYS_WAITPID (kernel SYS_WAIT uses a different in-kernel (pid,status) ABI). */
  return _check_err(syscall(SYS_WAITPID, (long)-1, wstatus, 0));
}

pid_t waitpid(pid_t pid, int *wstatus, int options) {
  return _check_err(syscall(SYS_WAITPID, pid, wstatus, options));
}

/* b1nix runs as root and has no login/passwd database; report "root" so tilde
 * (~) expansion in GNU Make's glob resolves to root's home. */
char *getlogin(void) {
  static char name[] = "root";
  return name;
}

int getgroups(int size, gid_t list[]) {
  (void)size;
  (void)list;
  return 0;
}

int seteuid(uid_t uid) {
  return _check_err(syscall(SYS_SETUID, uid));
}

int setegid(gid_t gid) {
  return _check_err(syscall(SYS_SETGID, gid));
}

uid_t getuid(void) {
  return (uid_t)syscall(SYS_GETUID);
}

uid_t geteuid(void) {
  return (uid_t)syscall(SYS_GETEUID);
}

gid_t getgid(void) {
  return (gid_t)syscall(SYS_GETGID);
}

gid_t getegid(void) {
  return (gid_t)syscall(SYS_GETEGID);
}

int setpgid(pid_t pid, pid_t pgid) {
  return _check_err(syscall(SYS_SETPGRP, pid, pgid));
}

#define B1NIX_TIOCGPGRP 0x540F
#define B1NIX_TIOCSPGRP 0x5410

pid_t getpgrp(void) {
  return (pid_t)syscall(SYS_GETPGRP);
}

pid_t tcgetpgrp(int fd) {
  pid_t pgrp = -1;
  if (ioctl(fd, B1NIX_TIOCGPGRP, &pgrp) < 0) {
    return -1;
  }
  return pgrp;
}

int tcsetpgrp(int fd, pid_t pgrp) {
  return ioctl(fd, B1NIX_TIOCSPGRP, &pgrp);
}

int tcgetattr(int fd, struct termios *termios_p) {
  return _check_err(syscall(SYS_TERMIOS_GET, fd, termios_p));
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
  (void)optional_actions;
  return _check_err(syscall(SYS_TERMIOS_SET, fd, termios_p));
}

pid_t setsid(void) {
  return _check_err(syscall(SYS_SETSID));
}

int getrlimit(int resource, struct rlimit *rlim) {
  (void)resource;
  (void)rlim;
  errno = ENOSYS;
  return -1;
}

#include <sys/uio.h>
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0)
      continue;
    ssize_t w = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (w < 0)
      return total > 0 ? total : -1;
    total += w;
    if ((size_t)w < iov[i].iov_len)
      break; /* short write: stop, report progress */
  }
  return total;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0)
      continue;
    ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
    if (r < 0)
      return total > 0 ? total : -1;
    total += r;
    if ((size_t)r < iov[i].iov_len)
      break; /* short read: stop */
  }
  return total;
}

#include <termios.h>
char *getpass(const char *prompt) {
  static char buf[256];
  if (prompt)
    write(2, prompt, strlen(prompt));
  /* Disable echo on stdin while reading the password. */
  struct termios old, raw;
  int have_tio = (tcgetattr(0, &old) == 0);
  if (have_tio) {
    raw = old;
    raw.c_lflag &= ~ECHO;
    tcsetattr(0, TCSANOW, &raw);
  }
  ssize_t n = read(0, buf, sizeof(buf) - 1);
  if (have_tio)
    tcsetattr(0, TCSANOW, &old);
  write(2, "\n", 1);
  if (n <= 0)
    return 0;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    n--;
  buf[n] = '\0';
  return buf;
}

int chown(const char *path, uid_t owner, gid_t group) {
  return _check_err(syscall(SYS_CHOWN, path, owner, group));
}

int fchown(int fd, uid_t owner, gid_t group) {
  return _check_err(syscall(SYS_FCHOWN, fd, owner, group));
}

int ttyname_r(int fd, char *buf, size_t buflen) {
  if (fd < 0) return EBADF;
  const char *nm = "/dev/tty";
  size_t n = strlen(nm);
  if (buflen <= n) return ERANGE;
  memcpy(buf, nm, n + 1);
  return 0;
}

char *ttyname(int fd) {
  static char buf[32];
  if (ttyname_r(fd, buf, sizeof(buf)) != 0)
    return 0;
  return buf;
}

int gethostname(char *name, size_t len) {
  if (!name || len == 0) {
    errno = EINVAL;
    return -1;
  }
  const char *host = "b1nix";
  /* Prefer /etc/hostname if present. */
  int fd = open("/etc/hostname", 0 /* O_RDONLY */);
  char filebuf[64];
  if (fd >= 0) {
    ssize_t r = read(fd, filebuf, sizeof(filebuf) - 1);
    close(fd);
    if (r > 0) {
      while (r > 0 && (filebuf[r - 1] == '\n' || filebuf[r - 1] == '\r'))
        r--;
      filebuf[r] = '\0';
      if (filebuf[0])
        host = filebuf;
    }
  }
  size_t hlen = strlen(host);
  if (hlen >= len) {
    memcpy(name, host, len - 1);
    name[len - 1] = '\0';
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(name, host, hlen + 1);
  return 0;
}

int sethostname(const char *name, size_t len) {
  (void)name;
  (void)len;
  return 0; /* b1nix hostname is fixed/boot-configured */
}

int setrlimit(int resource, const struct rlimit *rlim) {
  /* b1nix has no per-process resource limits; accept the request as a no-op so
   * callers that lower RLIMIT_CORE etc. (dropbear) proceed. */
  (void)resource;
  (void)rlim;
  return 0;
}

#include <syslog.h>
void openlog(const char *ident, int option, int facility) {
  (void)ident; (void)option; (void)facility;
}
void syslog(int priority, const char *format, ...) {
  (void)priority; (void)format;
}
void closelog(void) {
}
int setlogmask(int mask) {
  (void)mask;
  return 0;
}
