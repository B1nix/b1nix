#include "syscall.h"
#include <link.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <errno.h>
#include <sched.h>
#include <sys/klog.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <linux/futex.h>

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

int dup(int oldfd) {
  return _check_err(syscall(SYS_DUP, oldfd));
}

int access(const char *path, int mode) {
  return _check_err(syscall(SYS_ACCESS, path, mode));
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

int execlp(const char *file, const char *arg, ...) {
  va_list ap;

  /* Count the NULL-terminated trailing argument list (arg is argv[0]). */
  va_start(ap, arg);
  size_t n = 1;
  while (va_arg(ap, char *) != NULL)
    n++;
  va_end(ap);

  char *argv[n + 1];
  argv[0] = (char *)arg;
  va_start(ap, arg);
  for (size_t i = 1; i <= n; i++) /* i == n collects the terminating NULL */
    argv[i] = va_arg(ap, char *);
  va_end(ap);

  return execvp(file, argv);
}

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

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
  if (!timep || !result) return NULL;
  seconds_to_tm(*timep, result);
  result->tm_isdst = 0;
  return result;
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

struct tm *localtime_r(const time_t *timep, struct tm *result) {
  if (!timep || !result) return NULL;
  tzset();
  int std_offset_east = (int)(-timezone);
  int dst_on = daylight ? is_dst_active(*timep, std_offset_east) : 0;
  time_t local = *timep + (time_t)std_offset_east + (dst_on ? 3600 : 0);
  seconds_to_tm(local, result);
  result->tm_isdst = dst_on ? 1 : 0;
  return result;
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

int lstat(const char *path, struct stat *st) {
  return _check_err(syscall(SYS_LSTAT, path, st));
}

int fstat(int fd, struct stat *st) {
  return _check_err(syscall(SYS_FSTAT, fd, st));
}

int statfs(const char *path, struct statfs *buf) {
  return _check_err(syscall(SYS_STATFS, path, buf));
}

int fstatfs(int fd, struct statfs *buf) {
  return _check_err(syscall(SYS_FSTATFS, fd, buf));
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

int madvise(void *addr, size_t length, int advice) {
  return _check_err(syscall(SYS_MADVISE, addr, length, advice));
}

/* msync: b1nix has no MSYNC syscall and shared mmaps are write-through via the
 * page cache, so flushing dirty pages back to the backing file is implicit.
 * Treat msync as a successful no-op (callers like LLVM use it to ensure output
 * is on disk, which b1nix's write-back already guarantees on munmap/close). */
int msync(void *addr, size_t length, int flags) {
  (void)addr; (void)length; (void)flags;
  return 0;
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

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  return _check_err(syscall(SYS_POLL, fds, nfds, timeout));
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout_ts, const sigset_t *sigmask) {
  (void)sigmask;
  int timeout = -1;
  if (timeout_ts) {
    timeout = timeout_ts->tv_sec * 1000 + timeout_ts->tv_nsec / 1000000;
  }
  return _check_err(syscall(SYS_POLL, fds, nfds, timeout));
}

/* ── Extended attributes (M44 / BusyBox getfattr). The l*xattr variants pass
 * nofollow=1 so a terminal symlink is operated on rather than its target. ── */
int setxattr(const char *path, const char *name, const void *value,
             size_t size, int flags) {
  return _check_err(syscall(SYS_SETXATTR, path, name, value, size, flags, 0));
}
int lsetxattr(const char *path, const char *name, const void *value,
              size_t size, int flags) {
  return _check_err(syscall(SYS_SETXATTR, path, name, value, size, flags, 1));
}
ssize_t getxattr(const char *path, const char *name, void *value, size_t size) {
  return _check_err(syscall(SYS_GETXATTR, path, name, value, size, 0));
}
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) {
  return _check_err(syscall(SYS_GETXATTR, path, name, value, size, 1));
}
ssize_t listxattr(const char *path, char *list, size_t size) {
  return _check_err(syscall(SYS_LISTXATTR, path, list, size, 0));
}
ssize_t llistxattr(const char *path, char *list, size_t size) {
  return _check_err(syscall(SYS_LISTXATTR, path, list, size, 1));
}
int removexattr(const char *path, const char *name) {
  return _check_err(syscall(SYS_REMOVEXATTR, path, name, 0));
}
int lremovexattr(const char *path, const char *name) {
  return _check_err(syscall(SYS_REMOVEXATTR, path, name, 1));
}

int socket(int domain, int type, int protocol) {
  return _check_err(syscall(SYS_SOCKET, domain, type, protocol));
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
  return _check_err(syscall(SYS_SOCKETPAIR, domain, type, protocol, sv));
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

/* Single-iov scatter/gather sendmsg/recvmsg, enough for BusyBox `ip` (which
 * uses one iov over an AF_NETLINK socket). recvmsg leaves msg_namelen as the
 * caller set it — libnetlink pre-sets it to sizeof(sockaddr_nl) and only
 * checks that it is unchanged. */
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
  if (!msg || msg->msg_iovlen < 1) {
    errno = EINVAL;
    return -1;
  }
  long rc = syscall(SYS_SENDMSG, fd, msg, flags);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return (ssize_t)rc;
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
  if (!msg || msg->msg_iovlen < 1) {
    errno = EINVAL;
    return -1;
  }
  long rc = syscall(SYS_RECVMSG, fd, msg, flags);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return (ssize_t)rc;
}

int memfd_create(const char *name, unsigned int flags) {
  return _check_err(syscall(SYS_MEMFD_CREATE, name, flags));
}

/* POSIX named shared memory. b1nix has no /dev/shm namespace, so back this on
 * the anonymous shared-memory object (memfd) — a real fd supporting
 * ftruncate() + mmap(MAP_SHARED), which is all the standard contract (and the
 * sole caller, LLVM's Orc shared-memory mapper) needs. The name is used only as
 * a human-readable memfd label, not a lookup key; there is no namespace, so
 * shm_unlink() succeeds as a no-op. oflag/mode are accepted for source
 * compatibility (memfd ignores them — O_CREAT|O_EXCL can never collide here). */
int shm_open(const char *name, int oflag, mode_t mode) {
  (void)oflag;
  (void)mode;
  const char *label = name;
  if (label && label[0] == '/')
    label++; /* POSIX shm names start with '/'; strip it for the memfd label. */
  return memfd_create(label && label[0] ? label : "shm", MFD_CLOEXEC);
}

int shm_unlink(const char *name) {
  (void)name;
  return 0; /* No persistent namespace: the object dies with its last ref. */
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
  /* b1nix send() targets the socket's connected peer. POSIX sendto() with an
   * explicit destination sets that peer first (the kernel connect() on a
   * datagram/raw socket just records the default peer). This makes BusyBox
   * ping's per-packet sendto(raw_sock, ..., &dst) reach the target. */
  if (dest_addr && addrlen)
    connect(fd, dest_addr, addrlen);
  return send(fd, buf, len, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
  ssize_t n = recv(fd, buf, len, flags);
  /* SOCK_RAW/ICMP packets arrive with the IPv4 header included (version 4,
   * IHL 5, protocol 1 = ICMP); recover the peer address from that header so
   * recvfrom() callers like BusyBox ping get the real source. This is gated
   * tightly enough that it never misfires on a UDP datagram payload. */
  if (n >= 20 && src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
    const unsigned char *p = (const unsigned char *)buf;
    if ((p[0] & 0xF0) == 0x40 && (p[0] & 0x0F) == 0x05 && p[9] == 1) {
      struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
      sin->sin_family = AF_INET;
      sin->sin_port = 0;
      sin->sin_addr.s_addr = (unsigned int)p[12] | ((unsigned int)p[13] << 8) |
                             ((unsigned int)p[14] << 16) |
                             ((unsigned int)p[15] << 24);
      *addrlen = sizeof(struct sockaddr_in);
    }
  }
  return n;
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
  return (unsigned int)syscall(SYS_ALARM, seconds);
}

/* Map ITIMER_REAL to alarm() for BusyBox ping. */
int setitimer(int which, const struct itimerval *new_value,
              struct itimerval *old_value) {
  if (which == 0 /* ITIMER_REAL */) {
    if (new_value) {
      unsigned int sec = (unsigned int)new_value->it_value.tv_sec;
      if (sec == 0 && new_value->it_value.tv_usec > 0) {
        sec = 1;
      }
      alarm(sec);
    }
  }
  if (old_value) {
    old_value->it_interval.tv_sec = 0;
    old_value->it_interval.tv_usec = 0;
    old_value->it_value.tv_sec = 0;
    old_value->it_value.tv_usec = 0;
  }
  return 0;
}

int getitimer(int which, struct itimerval *curr_value) {
  (void)which;
  if (curr_value) {
    curr_value->it_interval.tv_sec = 0;
    curr_value->it_interval.tv_usec = 0;
    curr_value->it_value.tv_sec = 0;
    curr_value->it_value.tv_usec = 0;
  }
  return 0;
}

int fchmod(int fd, mode_t mode) {
  return _check_err(syscall(SYS_FCHMOD, fd, mode));
}

int ftruncate(int fd, off_t length) {
#ifdef __x86_64__
  return _check_err(syscall(SYS_FTRUNCATE, fd, (long)length));
#else
  return _check_err(syscall(SYS_FTRUNCATE, fd,
                            (long)(unsigned)(unsigned long long)length,
                            (long)(unsigned)((unsigned long long)length >> 32)));
#endif
}

#include <utime.h>
int utime(const char *filename, const struct utimbuf *times) {
  /* POSIX: a NULL times argument sets both atime and mtime to the current
   * time. Pass the two timestamps as scalar args (seconds) so the kernel
   * never has to copy a struct from user memory. */
  long long atime, mtime;
  if (times) {
    atime = (long long)times->actime;
    mtime = (long long)times->modtime;
  } else {
    atime = mtime = (long long)time(NULL);
  }
#ifdef __x86_64__
  /* One 64-bit register per scalar arg. */
  return _check_err(syscall(SYS_UTIME, filename, (long)atime, (long)mtime));
#else
  /* i386: int $0x80 arg registers are 32-bit, so each 64-bit timestamp is
   * passed as a lo/hi pair (the kernel reassembles them). A plain (long)
   * cast would truncate post-2038 timestamps to 32 bits. */
  return _check_err(syscall(SYS_UTIME, filename,
                            (long)(unsigned)(unsigned long long)atime,
                            (long)(unsigned)((unsigned long long)atime >> 32),
                            (long)(unsigned)(unsigned long long)mtime,
                            (long)(unsigned)((unsigned long long)mtime >> 32)));
#endif
}

int fork(void) {
  /* Run pthread_atfork() handlers around the fork (registry in posix_compat.c).
   * prepare runs before; child handlers run in the new child; parent handlers
   * run in the parent on both success and failure (to release prepare's locks). */
  extern void __atfork_prepare(void);
  extern void __atfork_parent(void);
  extern void __atfork_child(void);
  __atfork_prepare();
  long rc = syscall(SYS_FORK);
  if (rc == 0) {
    __atfork_child();
    return 0;
  }
  __atfork_parent();
  return _check_err(rc);
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

int killpg(int pgrp, int sig) {
  return kill(-pgrp, sig);
}

pid_t wait(int *wstatus) {
  /* POSIX wait(status) == waitpid(-1, status, 0): reap any child. Route via
   * SYS_WAITPID (kernel SYS_WAIT uses a different in-kernel (pid,status) ABI). */
  return _check_err(syscall(SYS_WAITPID, (long)-1, wstatus, 0));
}

pid_t waitpid(pid_t pid, int *wstatus, int options) {
  return _check_err(syscall(SYS_WAITPID, pid, wstatus, options));
}

int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options) {
  return _check_err(syscall(SYS_WAITID, (long)idtype, (long)id, infop, options));
}

/* wait4/wait3: BSD wait variants. b1nix has no rusage accounting at wait time,
 * so zero the rusage (if provided) and delegate to waitpid. */
pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage) {
  if (rusage) {
    char *p = (char *)rusage;
    for (unsigned i = 0; i < sizeof(struct rusage); i++) p[i] = 0;
  }
  return waitpid(pid, wstatus, options);
}

pid_t wait3(int *wstatus, int options, struct rusage *rusage) {
  return wait4((pid_t)-1, wstatus, options, rusage);
}

/* b1nix runs as root and has no login/passwd database; report "root" so tilde
 * (~) expansion in GNU Make's glob resolves to root's home. */
char *getlogin(void) {
  static char name[] = "root";
  return name;
}

int getgroups(int size, gid_t list[]) {
  return _check_err(syscall(SYS_GETGROUPS, size, list));
}

int seteuid(uid_t uid) {
  return _check_err(syscall(SYS_SETEUID, uid));
}

int setegid(gid_t gid) {
  return _check_err(syscall(SYS_SETEGID, gid));
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

pid_t getpgid(pid_t pid) {
  return _check_err(syscall(SYS_GETPGID, pid));
}

int setreuid(uid_t ruid, uid_t euid) {
  return _check_err(syscall(SYS_SETREUID, ruid, euid));
}

int setregid(gid_t rgid, gid_t egid) {
  return _check_err(syscall(SYS_SETREGID, rgid, egid));
}

int setresuid(uid_t ruid, uid_t euid, uid_t suid) {
  return _check_err(syscall(SYS_SETRESUID, ruid, euid, suid));
}

int setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
  return _check_err(syscall(SYS_SETRESGID, rgid, egid, sgid));
}

int getrusage(int who, struct rusage *usage) {
  return _check_err(syscall(SYS_GETRUSAGE, who, usage));
}

/* The kernel returns getpriority(2)'s Linux encoding 20 - nice (always
 * positive) so errnos stay unambiguous; convert back to POSIX -20..19. */
int getpriority(int which, id_t who) {
  (void)which; /* PRIO_PROCESS only */
  long rc = syscall(SYS_GETPRIORITY, who);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return 20 - (int)rc;
}

int setpriority(int which, id_t who, int prio) {
  (void)which; /* PRIO_PROCESS only */
  return _check_err(syscall(SYS_SETPRIORITY, who, prio));
}

int nice(int incr) {
  long rc = syscall(SYS_GETPRIORITY, 0);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  int next = (20 - (int)rc) + incr;
  if (next < -20)
    next = -20;
  if (next > 19)
    next = 19;
  if (setpriority(PRIO_PROCESS, 0, next) < 0)
    return -1;
  return next;
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

/* A descriptor is a terminal iff a TCGETS-class query succeeds on it. The
 * kernel returns an error (EINVAL/ENOTTY) for regular files, pipes and
 * non-tty devices, so this distinguishes a redirected fd from the console or
 * a pty — unlike the old "fd <= 2" placeholder, which mis-reported every
 * redirected stdio fd as a tty. */
int isatty(int fd) {
  struct termios t;
  if (tcgetattr(fd, &t) == 0)
    return 1;
  errno = ENOTTY;
  return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
  (void)optional_actions;
  return _check_err(syscall(SYS_TERMIOS_SET, fd, termios_p));
}

pid_t setsid(void) {
  return _check_err(syscall(SYS_SETSID));
}

pid_t getsid(pid_t pid) {
  return _check_err(syscall(SYS_GETSID, pid));
}

/* POSIX tcgetsid(): the session ID of the session for which `fd` is the
 * controlling terminal. b1nix has no per-tty session query (no TIOCGSID), so
 * this is a best-effort: fd must be a terminal (else ENOTTY), and the answer is
 * the caller's own session — correct whenever fd is the caller's controlling
 * terminal, which is how getty/login use it. */
pid_t tcgetsid(int fd) {
  if (!isatty(fd)) {
    errno = ENOTTY;
    return -1;
  }
  return getsid(0);
}

int getrlimit(int resource, struct rlimit *rlim) {
  return _check_err(syscall(SYS_GETRLIMIT, resource, rlim));
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
  return _check_err(syscall(SYS_SETRLIMIT, resource, rlim));
}

#include <syslog.h>
#include <stdio.h>

static int log_fd = -1;
static const char *log_ident = NULL;
static int log_option = 0;
static int log_facility = LOG_USER;
static int log_mask = 0xff;

static void connect_log(void) {
  if (log_fd >= 0) return;
  /* ponytail: /dev/log is a kernel char-device sink (forwards to the kernel
   * log), not an AF_UNIX socket — b1nix runs no userspace syslogd. If one is
   * ever added, switch back to socket(AF_UNIX, SOCK_DGRAM)+connect here. */
  log_fd = open("/dev/log", O_WRONLY | O_CLOEXEC);
}

void openlog(const char *ident, int option, int facility) {
  log_ident = ident;
  log_option = option;
  log_facility = facility;
  if (option & LOG_NDELAY) {
    connect_log();
  }
}

void syslog(int priority, const char *format, ...) {
  if (!(log_mask & LOG_MASK(LOG_PRI(priority)))) return;

  int pri = priority;
  if ((pri & LOG_FACMASK) == 0) {
    pri |= log_facility;
  }

  char new_fmt[1024];
  int i = 0, j = 0;
  int saved_errno = errno;
  while (format[i] && j < 1022) {
    if (format[i] == '%' && format[i+1] == 'm') {
      const char *err = strerror(saved_errno);
      if (!err) err = "Unknown error";
      size_t len = strlen(err);
      if (j + len < 1023) {
        memcpy(new_fmt + j, err, len);
        j += len;
      }
      i += 2;
    } else {
      new_fmt[j++] = format[i++];
    }
  }
  new_fmt[j] = '\0';

  char msg[2048];
  va_list ap;
  va_start(ap, format);
  vsnprintf(msg, sizeof(msg), new_fmt, ap);
  va_end(ap);

  char buf[4096];
  int len = 0;
  len += snprintf(buf + len, sizeof(buf) - len, "<%d>", pri);

  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);
  if (tm_info) {
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%b %d %H:%M:%S ", tm_info);
    len += snprintf(buf + len, sizeof(buf) - len, "%s", time_str);
  }

  if (log_ident) {
    len += snprintf(buf + len, sizeof(buf) - len, "%s", log_ident);
  }
  if (log_option & LOG_PID) {
    len += snprintf(buf + len, sizeof(buf) - len, "[%d]", getpid());
  }
  if (log_ident || (log_option & LOG_PID)) {
    len += snprintf(buf + len, sizeof(buf) - len, ": ");
  }
  len += snprintf(buf + len, sizeof(buf) - len, "%s", msg);

  if (log_fd < 0) {
    connect_log();
  }

  int sent = -1;
  if (log_fd >= 0) {
    sent = write(log_fd, buf, len);
    if (sent < 0) {
      close(log_fd);
      log_fd = -1;
      connect_log();
      if (log_fd >= 0) {
        sent = write(log_fd, buf, len);
      }
    }
  }

  if (sent < 0) {
    if (log_option & LOG_CONS) {
      int cfd = open("/dev/console", O_WRONLY | O_NOCTTY);
      if (cfd >= 0) {
        write(cfd, buf, len);
        write(cfd, "\r\n", 2);
        close(cfd);
      }
    }
  }

  if (log_option & LOG_PERROR) {
    write(2, buf, len);
    write(2, "\n", 1);
  }
}

void closelog(void) {
  if (log_fd >= 0) {
    close(log_fd);
    log_fd = -1;
  }
}

int setlogmask(int mask) {
  int old = log_mask;
  if (mask != 0) {
    log_mask = mask;
  }
  return old;
}

int utimes(const char *filename, const struct timeval times[2]) {
  if (!times) {
    return utime(filename, NULL);
  }
  struct utimbuf buf;
  buf.actime = times[0].tv_sec;
  buf.modtime = times[1].tv_sec;
  return utime(filename, &buf);
}

int mknod(const char *pathname, mode_t mode, dev_t dev) {
  (void)pathname;
  (void)mode;
  (void)dev;
  errno = ENOSYS;
  return -1;
}

int clock_settime(int clk_id, const struct timespec *tp) {
  if (!tp) { errno = EINVAL; return -1; }
  if (clk_id != CLOCK_REALTIME) { errno = EINVAL; return -1; }
  struct timeval tv = { .tv_sec = tp->tv_sec, .tv_usec = tp->tv_nsec / 1000 };
  return _check_err(syscall(SYS_SETTIMEOFDAY, &tv));
}

char *strptime(const char *buf, const char *format, struct tm *tm) {
  const char *bp = buf;
  const char *fp = format;

  while (*fp) {
    if (*fp == '%') {
      fp++;
      if (*fp == '\0') return NULL;

      switch (*fp) {
        case '%': {
          if (*bp != '%') return NULL;
          bp++;
          break;
        }
        case 'Y': {
          int val = 0;
          int len = 0;
          while (*bp >= '0' && *bp <= '9' && len < 4) {
            val = val * 10 + (*bp - '0');
            bp++;
            len++;
          }
          if (len == 0) return NULL;
          tm->tm_year = val - 1900;
          break;
        }
        case 'y': {
          int val = 0;
          int len = 0;
          while (*bp >= '0' && *bp <= '9' && len < 2) {
            val = val * 10 + (*bp - '0');
            bp++;
            len++;
          }
          if (len == 0) return NULL;
          if (val < 69) val += 100;
          tm->tm_year = val;
          break;
        }
        case 'm': {
          int val = 0;
          int len = 0;
          while (*bp >= '0' && *bp <= '9' && len < 2) {
            val = val * 10 + (*bp - '0');
            bp++;
            len++;
          }
          if (len == 0 || val < 1 || val > 12) return NULL;
          tm->tm_mon = val - 1;
          break;
        }
        case 'd':
        case 'e': {
          if (*fp == 'e' && *bp == ' ') bp++;
          int val = 0;
          int len = 0;
          while (*bp >= '0' && *bp <= '9' && len < 2) {
            val = val * 10 + (*bp - '0');
            bp++;
            len++;
          }
          if (len == 0 || val < 1 || val > 31) return NULL;
          tm->tm_mday = val;
          break;
        }
        case 'H': {
          int val = 0;
          int len = 0;
          while (*bp >= '0' && *bp <= '9' && len < 2) {
            val = val * 10 + (*bp - '0');
            bp++;
            len++;
          }
          if (len == 0 || val < 0 || val > 23) return NULL;
          tm->tm_hour = val;
          break;
        }
        case 'M': {
          int val = 0;
          int len = 0;
          while (*bp >= '0' && *bp <= '9' && len < 2) {
            val = val * 10 + (*bp - '0');
            bp++;
            len++;
          }
          if (len == 0 || val < 0 || val > 59) return NULL;
          tm->tm_min = val;
          break;
        }
        case 'S': {
          int val = 0;
          int len = 0;
          while (*bp >= '0' && *bp <= '9' && len < 2) {
            val = val * 10 + (*bp - '0');
            bp++;
            len++;
          }
          if (len == 0 || val < 0 || val > 60) return NULL;
          tm->tm_sec = val;
          break;
        }
        default: {
          return NULL;
        }
      }
      fp++;
    } else if (*fp == ' ' || *fp == '\t' || *fp == '\r' || *fp == '\n') {
      while (*bp == ' ' || *bp == '\t' || *bp == '\r' || *bp == '\n') {
        bp++;
      }
      fp++;
    } else {
      if (*bp != *fp) return NULL;
      bp++;
      fp++;
    }
  }
  return (char *)bp;
}

int symlink(const char *target, const char *linkpath) {
  return _check_err(syscall(SYS_SYMLINK, target, linkpath));
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
  return _check_err(syscall(SYS_READLINK, pathname, buf, bufsiz));
}

int lchown(const char *path, uid_t owner, gid_t group) {
  struct stat st;
  if (lstat(path, &st) < 0) {
    return -1;
  }
  if (S_ISLNK(st.st_mode)) {
    /* b1nix kernel has no lchown(2) syscall variant; symlink ownership
     * changes are not supported. EPERM is the standard error for an
     * operation that is blocked by kernel policy (cf. Linux ENOSYS vs
     * EPERM distinction for privilege-gated calls). */
    errno = EPERM;
    return -1;
  }
  return chown(path, owner, group);
}

int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags) {
  if (dirfd != AT_FDCWD) {
    if (pathname && pathname[0] == '/') {
      /* absolute path */
    } else {
      errno = ENOSYS;
      return -1;
    }
  }

  long long actime, modtime;
  if (!times) {
    actime = modtime = time(NULL);
  } else {
    struct timespec now;
    int has_now = (times[0].tv_nsec == UTIME_NOW || times[1].tv_nsec == UTIME_NOW);
    if (has_now) {
      clock_gettime(CLOCK_REALTIME, &now);
    }

    struct stat st;
    int has_omit = (times[0].tv_nsec == UTIME_OMIT || times[1].tv_nsec == UTIME_OMIT);
    if (has_omit) {
      int stat_res = (flags & AT_SYMLINK_NOFOLLOW) ? lstat(pathname, &st) : stat(pathname, &st);
      if (stat_res < 0) {
        return -1;
      }
    }

    if (times[0].tv_nsec == UTIME_NOW) {
      actime = now.tv_sec;
    } else if (times[0].tv_nsec == UTIME_OMIT) {
      actime = st.st_atime;
    } else {
      actime = times[0].tv_sec;
    }

    if (times[1].tv_nsec == UTIME_NOW) {
      modtime = now.tv_sec;
    } else if (times[1].tv_nsec == UTIME_OMIT) {
      modtime = st.st_mtime;
    } else {
      modtime = times[1].tv_sec;
    }
  }

  struct utimbuf buf;
  buf.actime = actime;
  buf.modtime = modtime;
  return utime(pathname, &buf);
}

int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
  if (dirfd != AT_FDCWD) {
    if (pathname && pathname[0] == '/') {
      /* absolute path */
    } else {
      errno = ENOSYS;
      return -1;
    }
  }
  if (flags & AT_SYMLINK_NOFOLLOW) {
    return lstat(pathname, statbuf);
  }
  return stat(pathname, statbuf);
}

int unlinkat(int dirfd, const char *pathname, int flags) {
  if (dirfd != AT_FDCWD) {
    if (pathname && pathname[0] == '/') {
      /* absolute path */
    } else {
      errno = ENOSYS;
      return -1;
    }
  }
  if (flags & AT_REMOVEDIR) {
    return rmdir(pathname);
  }
  return unlink(pathname);
}

int openat(int dirfd, const char *path, int flags, ...) {
  unsigned int mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, unsigned int);
    va_end(ap);
  }
  if (dirfd != AT_FDCWD) {
    if (path && path[0] == '/') {
      /* absolute path */
    } else {
      errno = ENOSYS;
      return -1;
    }
  }
  return open(path, flags, mode);
}

int futimens(int fd, const struct timespec times[2]) {
  /* Resolve fd → path via /proc/self/fd/<N> and delegate to utimensat.
   * This is the standard fallback for systems without a futimens(2)
   * syscall (b1nix has SYS_UTIME but not a direct fd-based variant). */
  char link[64];
  char path[4096];
  snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  ssize_t len = readlink(link, path, sizeof(path) - 1);
  if (len < 0) {
    errno = EBADF;
    return -1;
  }
  path[len] = '\0';
  return utimensat(AT_FDCWD, path, times, 0);
}

int uname(struct utsname *buf) {
  return _check_err(syscall(SYS_UNAME, buf));
}

int sigsuspend(const sigset_t *mask) {
  if (mask) {
    return _check_err(syscall(SYS_SIGSUSPEND, mask));
  }
  errno = EINVAL;
  return -1;
}

long sysconf(int name) {
  switch (name) {
  case _SC_CLK_TCK:
    return 100;
  case _SC_PAGESIZE:
    return 4096;
  case _SC_NPROCESSORS_CONF:
  case _SC_NPROCESSORS_ONLN:
    /* ponytail: report 1 — no userspace CPU-count primitive yet, and a single
     * worker is plenty for a software renderer. Wire to /sys/.../cpu/online if
     * a port ever needs real parallelism. */
    return 1;
  case _SC_PHYS_PAGES:
  case _SC_AVPHYS_PAGES: {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
      unsigned long unit = si.mem_unit ? si.mem_unit : 1;
      unsigned long ram = (name == _SC_PHYS_PAGES) ? si.totalram : si.freeram;
      return (long)((ram * unit) / 4096UL);
    }
    return -1;
  }
  case _SC_ARG_MAX:
    return 131072; /* matches ARG_MAX */
  case _SC_GETPW_R_SIZE_MAX:
    return 1024;
  default:
    errno = EINVAL;
    return -1;
  }
}

int getpagesize(void) { return (int)sysconf(_SC_PAGESIZE); }

clock_t times(struct tms *buf) {
  long rc = syscall(SYS_TIMES, buf);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return (clock_t)-1;
  }
  return (clock_t)rc;
}

/* vfork() is intentionally an alias for fork(), which POSIX explicitly permits
 * ("vfork() may behave like fork()"). A true vfork — child borrows the parent's
 * address space and stack while the parent is suspended until execve()/_exit()
 * — needs a kernel CLONE_VFORK (parent-suspend) primitive that b1nix does not
 * implement, plus a hand-written assembly wrapper (the C frame must not unwind
 * the shared stack the parent will resume on). The performance argument is also
 * weak here: b1nix fork() is copy-on-write (paging_clone_address_space marks
 * pages read-only rather than copying them), so the only cost vfork would save
 * is duplicating the page-table hierarchy — not the page contents. The risk to
 * process launch / TCC of getting shared-stack semantics subtly wrong far
 * outweighs that, so the alias stands by design. */
pid_t vfork(void) {
  return fork();
}

int fchdir(int fd) {
  return _check_err(syscall(SYS_FCHDIR, fd));
}

int chroot(const char *path) {
  (void)path;
  errno = EPERM;
  return -1;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz) {
  (void)tz; /* timezone is obsolete and ignored, as on Linux. */
  if (!tv) { errno = EINVAL; return -1; }
  return _check_err(syscall(SYS_SETTIMEOFDAY, tv));
}

/* klogctl(): the Linux syslog(2) interface BusyBox dmesg uses. b1nix exposes
 * the kernel ring buffer through SYS_DMESG (read-only), so map the read
 * actions onto it and answer the size/console queries locally. KLOG_BUF_SIZE
 * mirrors <b1nix/klog.h> (64 KiB). */
int klogctl(int type, char *bufp, int len) {
  switch (type) {
  case SYSLOG_ACTION_READ_ALL:   /* 3 */
  case SYSLOG_ACTION_READ_CLEAR: /* 4: b1nix has no clear-on-read; same as 3 */
    if (!bufp || len <= 0) {
      errno = EINVAL;
      return -1;
    }
    return _check_err(syscall(SYS_DMESG, bufp, (size_t)len));
  case SYSLOG_ACTION_SIZE_BUFFER: /* 10 */
    return 65536;
  case SYSLOG_ACTION_SIZE_UNREAD: /* 9 */
    return 0;
  case SYSLOG_ACTION_CONSOLE_OFF:   /* 6 */
  case SYSLOG_ACTION_CONSOLE_ON:    /* 7 */
  case SYSLOG_ACTION_CONSOLE_LEVEL: /* 8: no console loglevel in b1nix */
  case SYSLOG_ACTION_CLOSE:         /* 0 */
  case SYSLOG_ACTION_OPEN:          /* 1 */
  case SYSLOG_ACTION_CLEAR:         /* 5 */
    return 0;
  default:
    errno = EINVAL;
    return -1;
  }
}

int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data) {
  (void)data; /* b1nix SYS_MOUNT takes no fs-specific data argument */
  return _check_err(syscall(SYS_MOUNT, source, target, filesystemtype,
                            (long)mountflags));
}

int umount(const char *target) {
  return _check_err(syscall(SYS_UMOUNT, target));
}

int umount2(const char *target, int flags) {
  (void)flags; /* b1nix has no lazy/force unmount distinction yet */
  return _check_err(syscall(SYS_UMOUNT, target));
}

int usleep(unsigned int usec) {
  struct timespec req;
  req.tv_sec = (time_t)(usec / 1000000u);
  req.tv_nsec = (long)(usec % 1000000u) * 1000L;
  return nanosleep(&req, 0);
}

int sysinfo(struct sysinfo *info) {
  if (!info) {
    errno = EFAULT;
    return -1;
  }
  return _check_err(syscall(SYS_SYSINFO, info));
}

/* sched_yield(2): hand the CPU to another runnable task. Backs the C11
 * thrd_yield() that Mesa and other threaded ports use. */
int sched_yield(void) {
  syscall(SYS_YIELD);
  return 0;
}

/* clock_nanosleep(2): relative (flags==0) defers to nanosleep; TIMER_ABSTIME
 * sleeps until the absolute deadline by subtracting the current clock. */
int clock_nanosleep(int clk_id, int flags, const struct timespec *req,
                    struct timespec *rem) {
  if (!req)
    return EINVAL;
  if (flags == 0)
    return nanosleep(req, rem) == 0 ? 0 : errno;
  struct timespec now, delta;
  if (clock_gettime(clk_id, &now) != 0)
    return errno;
  delta.tv_sec = req->tv_sec - now.tv_sec;
  delta.tv_nsec = req->tv_nsec - now.tv_nsec;
  if (delta.tv_nsec < 0) { delta.tv_sec--; delta.tv_nsec += 1000000000L; }
  if (delta.tv_sec < 0) return 0;
  return nanosleep(&delta, rem) == 0 ? 0 : errno;
}

/* Single scheduling class: degenerate priority range. */
int sched_get_priority_max(int policy) { (void)policy; return 0; }
int sched_get_priority_min(int policy) { (void)policy; return 0; }

/* Chromium port: b1nix has nice-based scheduling only — no SCHED_FIFO/RR
 * realtime classes. Report SCHED_OTHER honestly; accept setting SCHED_OTHER and
 * reject the realtime policies with EINVAL (POSIX); getparam reports priority 0.
 * (b1nix is single-policy, so pid is not validated against live tasks here.) */
int sched_getscheduler(int pid) { (void)pid; return SCHED_OTHER; }

int sched_setscheduler(int pid, int policy, const struct sched_param *param) {
  (void)pid; (void)param;
  if (policy == SCHED_OTHER) return 0;
  errno = EINVAL;
  return -1;
}

int sched_getparam(int pid, struct sched_param *param) {
  (void)pid;
  if (!param) { errno = EINVAL; return -1; }
  param->sched_priority = 0;
  return 0;
}

int sched_getcpu(void) { return getcpu(); }

/* sched_getaffinity(2): fill `mask` with the set of CPUs the task may run on
 * (all online CPUs on b1nix). Returns 0 on success. */
int sched_getaffinity(int pid, size_t cpusetsize, cpu_set_t *mask) {
  long rc = syscall(SYS_SCHED_GETAFFINITY, pid, cpusetsize, mask);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return 0;
}

/* flock(2) whole-file advisory lock, backed by fcntl record locking. */
int flock(int fd, int operation) {
  struct flock fl;
  fl.l_type = (operation & LOCK_UN)   ? F_UNLCK
              : (operation & LOCK_EX) ? F_WRLCK
                                      : F_RDLCK;
  fl.l_whence = 0; /* SEEK_SET */
  fl.l_start = 0;
  fl.l_len = 0; /* whole file */
  return fcntl(fd, (operation & LOCK_NB) ? F_SETLK : F_SETLKW, &fl);
}

/* fenv: b1nix has a fixed userspace FPU mode and no exception-flag plumbing. */
#include <fenv.h>
int feclearexcept(int e) { (void)e; return 0; }
int feraiseexcept(int e) { (void)e; return 0; }
int fetestexcept(int e) { (void)e; return 0; }
/* prctl: b1nix has no prctl syscall; the operations callers use (thread/VMA
 * naming) are cosmetic, so report success without doing anything. */
int prctl(int option, ...) { (void)option; return 0; }
int fegetexceptflag(fexcept_t *flagp, int e) { (void)e; if (flagp) *flagp = 0; return 0; }
int fesetexceptflag(const fexcept_t *flagp, int e) { (void)flagp; (void)e; return 0; }
int fegetround(void) { return 0x000 /* FE_TONEAREST */; }
int fesetround(int r) { (void)r; return 0; }
/* Real save/restore of the x86_64 FP environment (x87 via fnstenv/fldenv + SSE
 * MXCSR via stmxcsr/ldmxcsr). These MUST be real, not stubs: openlibm's
 * nearbyint/rint call fegetenv() and then do an INLINE `fldenv` over the saved
 * 28-byte x87 env. A stub that wrote only a zero control word left the env's FCW
 * at 0 (all x87 exceptions UNMASKED) + the rest stack garbage, so the inline
 * fldenv reasserted a pending exception and raised #MF (V8 TurboFan hit this via
 * nearbyint during optimized codegen). fnstenv masks exceptions as a side
 * effect, so reload the just-saved env to leave the live FPU as it was. */
int fegetenv(fenv_t *envp) {
  if (envp) {
    __asm__ volatile("fnstenv %0; fldenv %0" : "+m"(*envp));
    __asm__ volatile("stmxcsr %0" : "=m"(envp->__mxcsr));
  }
  return 0;
}
int fesetenv(const fenv_t *envp) {
  if (envp) {
    __asm__ volatile("fldenv %0" : : "m"(*envp));
    __asm__ volatile("ldmxcsr %0" : : "m"(envp->__mxcsr));
  }
  return 0;
}
/* Save the env, then mask + clear all exceptions (x87 and SSE) so subsequent FP
 * ops can't trap until feupdateenv() restores. */
int feholdexcept(fenv_t *envp) {
  if (envp) {
    __asm__ volatile("fnstenv %0" : "=m"(*envp));
    __asm__ volatile("fnclex");
    unsigned int mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(envp->__mxcsr));
    mxcsr = (envp->__mxcsr | 0x1F80u) & ~0x3Fu; /* mask all, clear flags */
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
  }
  return 0;
}
int feupdateenv(const fenv_t *envp) { return fesetenv(envp); }

/* openlibm's fma() calls __isnormal; provide the glibc-style classifier. */
int __isnormal(double x) { return __builtin_isnormal(x); }
int __isnormalf(float x) { return __builtin_isnormal(x); }

/* The prebuilt libstdc++ (built --without-headers) emits a C++-mangled
 * reference to _exit from its debug code. Alias the mangled name to the real
 * C _exit so it links; the debug path is unused at runtime. */
__attribute__((noreturn)) void __b1nix_exit_cxx(int status) __asm__("_Z5_exiti");
__attribute__((noreturn)) void __b1nix_exit_cxx(int status) { _exit(status); }

/* Stack-protector runtime. V8 (and other ports) compile with -fstack-protector,
 * which references these. Fixed non-zero canary; __stack_chk_fail aborts. */
unsigned long __stack_chk_guard = 0x595e9fbd94fda766UL;
void __stack_chk_fail(void) {
  static const char smsg[] = "*** stack smashing detected ***\n";
  write(2, smsg, sizeof(smsg) - 1);
  abort();
}

/* pread via save/seek/read/restore. b1nix has no pread syscall; this is not
 * atomic w.r.t. concurrent seeks on the same fd, but matches pread's "don't
 * change the offset" contract for the single-fd-owner case (PA crash reader). */
ssize_t pread(int fd, void *buf, size_t n, off_t offset) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  ssize_t r = read(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}

/* b1nix has no mremap syscall; report failure so callers fall back to
 * munmap+mmap. (V8 perf-jit references it; that path is dead under --jitless.) */
void *mremap(void *old_address, size_t old_size, size_t new_size, int flags,
             ...) {
  (void)old_address;
  (void)old_size;
  (void)new_size;
  (void)flags;
  errno = ENOMEM;
  return MAP_FAILED;
}

/* b1nix is statically linked with no dynamic loader: report zero shared objects
 * (don't invoke the callback). Symbolizers/backtrace code degrade gracefully. */
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *),
                    void *data) {
  (void)callback;
  (void)data;
  return 0;
}

/* Real syscall() function (Linux-compat). Defined last, after all the macro
 * users above; #undef the fast macro so this is a genuine callable symbol.
 * Reads up to 6 args (extra args past what a given syscall uses are ignored by
 * the kernel, matching glibc's syscall()). */
#undef syscall
/* Linux x86_64 syscall numbers that the Rust standard library issues directly
 * via libc::syscall() (the libc crate hard-codes Linux numbers). b1nix has its
 * own SYS_* enum, so syscall() must recognize these Linux numbers and route
 * them to the native b1nix syscall. This mirrors b1nix's existing M40 Linux-ABI
 * layer (which already remaps Linux signal numbers, sigsets, etc.). */
#define LINUX_SYS_futex 202

/* Translate a Linux-style futex() call into b1nix's native SYS_FUTEX.
 *
 * Rust std (std::sys::pal::unix::futex) issues:
 *   wait:  syscall(202, uaddr, FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG, val,
 *                  *abs_timespec_or_null, NULL, FUTEX_BITSET_MATCH_ANY)
 *   wake:  syscall(202, uaddr, FUTEX_WAKE|FUTEX_PRIVATE_FLAG, count)
 * b1nix's native ABI is:
 *   SYS_FUTEX(uaddr, op{0=WAIT,1=WAKE}, val, timeout_ms)
 * The op gets its FUTEX_PRIVATE_FLAG masked off; the absolute CLOCK_MONOTONIC
 * timespec is converted to a relative millisecond timeout. The return follows
 * the libc convention (-1 + errno on error) so std's `errno`-based dispatch
 * works. */
static long _futex_bridge(long uaddr, long op_full, long val, long ts_ptr) {
  int op = (int)(op_full & ~FUTEX_PRIVATE_FLAG);
  long timeout_ms = 0;

  if (op == FUTEX_WAIT_BITSET || op == FUTEX_WAIT) {
    op = FUTEX_WAIT;
    const struct timespec *abs = (const struct timespec *)ts_ptr;
    if (abs) {
      /* std passes an ABSOLUTE CLOCK_MONOTONIC deadline; b1nix wants a relative
       * millisecond timeout. Compute (deadline - now), clamped to >= 1ms. */
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        now.tv_sec = 0;
        now.tv_nsec = 0;
      }
      long long ms = (long long)(abs->tv_sec - now.tv_sec) * 1000 +
                     ((long long)abs->tv_nsec - (long long)now.tv_nsec) / 1000000;
      if (ms <= 0) ms = 1; /* already past: still take one short wait */
      timeout_ms = (long)ms;
    }
  } else if (op == FUTEX_WAKE_BITSET || op == FUTEX_WAKE) {
    op = FUTEX_WAKE;
    timeout_ms = 0;
  } else {
    errno = ENOSYS;
    return -1;
  }

  long rc = _syscall_raw(SYS_FUTEX, uaddr, op, val, timeout_ms, 0, 0);
  if (rc < 0) {
    errno = normalize_errno(rc);
    return -1;
  }
  return rc;
}

long syscall(long number, ...) {
  va_list ap;
  va_start(ap, number);
  long a0 = va_arg(ap, long);
  long a1 = va_arg(ap, long);
  long a2 = va_arg(ap, long);
  long a3 = va_arg(ap, long);
  long a4 = va_arg(ap, long);
  long a5 = va_arg(ap, long);
  va_end(ap);

  /* Linux-ABI number remap for the few syscalls Rust std issues by raw number.
   * SYS_FUTEX needs argument-shape translation, not just a number swap. */
  if (number == LINUX_SYS_futex)
    return _futex_bridge(a0, a1, a2, a3);

  return _syscall_raw(number, a0, a1, a2, a3, a4, a5);
}
