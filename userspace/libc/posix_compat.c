/* posix_compat.c — POSIX/Linux libc surface needed by foreign-function callers
 * (Rust std's `libc` FFI, ports) that b1nix's other libc files didn't yet
 * export. Everything here is a real implementation over b1nix syscalls; the few
 * operations b1nix's kernel genuinely lacks (splice, sigwait) report ENOSYS,
 * which is the documented contract that makes callers fall back rather than a
 * faked success.
 *
 * Also provides real (non-inline) definitions of getpid/getppid/chmod/rename,
 * which were previously `static inline` in the public headers — inline-only
 * functions emit no linkable symbol, so FFI consumers could not resolve them.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <syscall.h>

#ifndef S_IFIFO
#define S_IFIFO 0010000
#endif

/* splice lives in <fcntl.h> on Linux; declare here so we export the symbol. */
ssize_t splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
               size_t len, unsigned int flags);

/* ── De-inlined basic syscall wrappers (real exported symbols) ── */

int getpid(void) { return (int)syscall(SYS_GETPID); }
int getppid(void) { return (int)syscall(SYS_GETPPID); }

int chmod(const char *path, unsigned int mode) {
  long rc = syscall(SYS_CHMOD, path, mode);
  if (rc < 0) { errno = normalize_errno(rc); return -1; }
  return 0;
}

int rename(const char *oldpath, const char *newpath) {
  long rc = syscall(SYS_RENAME, oldpath, newpath);
  if (rc < 0) { errno = normalize_errno(rc); return -1; }
  return 0;
}

/* ── fd / pipe helpers ── */

int pipe2(int pipefd[2], int flags) {
  if (pipe(pipefd) != 0) return -1;
  if (flags & O_CLOEXEC) {
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
  }
  if (flags & O_NONBLOCK) {
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
  }
  return 0;
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
  int c = accept(sockfd, addr, addrlen);
  if (c < 0) return -1;
  if (flags & SOCK_CLOEXEC) fcntl(c, F_SETFD, FD_CLOEXEC);
  if (flags & SOCK_NONBLOCK) fcntl(c, F_SETFL, O_NONBLOCK);
  return c;
}

/* b1nix has no fdatasync syscall; a full fsync (data + metadata) satisfies the
 * weaker data-sync contract. */
int fdatasync(int fd) { return fsync(fd); }

int mkfifo(const char *pathname, mode_t mode) {
  return mknod(pathname, (mode & 07777) | S_IFIFO, 0);
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
           int flags) {
  (void)flags;
  if (olddirfd == AT_FDCWD && newdirfd == AT_FDCWD) return link(oldpath, newpath);
  errno = ENOSYS;
  return -1;
}

/* ── positioned I/O (b1nix has no pread/pwrite syscalls: seek/op/restore) ── */

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  ssize_t r = write(fd, buf, count);
  lseek(fd, cur, SEEK_SET);
  return r;
}

ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  ssize_t r = readv(fd, iov, iovcnt);
  lseek(fd, cur, SEEK_SET);
  return r;
}

ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
  off_t cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  ssize_t r = writev(fd, iov, iovcnt);
  lseek(fd, cur, SEEK_SET);
  return r;
}

/* Real read/write copy loop (b1nix has no zero-copy sendfile syscall). */
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
  char buf[16384];
  size_t total = 0;
  off_t off = offset ? *offset : 0;
  while (count > 0) {
    size_t want = count < sizeof(buf) ? count : sizeof(buf);
    ssize_t r = offset ? pread(in_fd, buf, want, off) : read(in_fd, buf, want);
    if (r < 0) return total ? (ssize_t)total : -1;
    if (r == 0) break;
    ssize_t w = 0;
    while (w < r) {
      ssize_t k = write(out_fd, buf + w, (size_t)(r - w));
      if (k < 0) {
        if (offset) *offset = off + w;
        return (total + w) ? (ssize_t)(total + w) : -1;
      }
      w += k;
    }
    total += (size_t)w;
    off += w;
    count -= (size_t)w;
    if (r < (ssize_t)want) break;
  }
  if (offset) *offset = off;
  return (ssize_t)total;
}

/* b1nix has no splice (pipe-coupled zero-copy) primitive; report ENOSYS so
 * callers (e.g. Rust std's io::copy) fall back to a userspace copy. */
ssize_t splice(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
               size_t len, unsigned int flags) {
  (void)fd_in; (void)off_in; (void)fd_out; (void)off_out; (void)len; (void)flags;
  errno = ENOSYS;
  return -1;
}

/* b1nix has no synchronous signal-wait primitive. Report ENOSYS (sigwait
 * returns the error number directly). */
int sigwait(const sigset_t *set, int *sig) {
  (void)set; (void)sig;
  return ENOSYS;
}

/* Weak so a program that bundles its own daemon() (e.g. dropbear, whose cached
 * autoconf was generated before b1nix libc had one, so its config.h leaves
 * HAVE_DAEMON undefined and it compiles its own) wins the link instead of
 * colliding with this definition. */
__attribute__((weak)) int daemon(int nochdir, int noclose) {
  int r = fork();
  if (r < 0) return -1;
  if (r > 0) _exit(0);
  if (setsid() < 0) return -1;
  if (!nochdir) { if (chdir("/") != 0) {} }
  if (!noclose) {
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
      dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
      if (fd > 2) close(fd);
    }
  }
  return 0;
}

/* ── pthread_atfork ── handlers run around fork() (see libc fork()). */

#define ATFORK_MAX 32
static struct {
  void (*prepare)(void);
  void (*parent)(void);
  void (*child)(void);
} g_atfork[ATFORK_MAX];
static int g_atfork_count;

int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                   void (*child)(void)) {
  if (g_atfork_count >= ATFORK_MAX) return ENOMEM;
  g_atfork[g_atfork_count].prepare = prepare;
  g_atfork[g_atfork_count].parent = parent;
  g_atfork[g_atfork_count].child = child;
  g_atfork_count++;
  return 0;
}

/* Called by fork() in unistd.c. prepare handlers run LIFO; parent/child FIFO. */
void __atfork_prepare(void) {
  for (int i = g_atfork_count - 1; i >= 0; i--)
    if (g_atfork[i].prepare) g_atfork[i].prepare();
}
void __atfork_parent(void) {
  for (int i = 0; i < g_atfork_count; i++)
    if (g_atfork[i].parent) g_atfork[i].parent();
}
void __atfork_child(void) {
  for (int i = 0; i < g_atfork_count; i++)
    if (g_atfork[i].child) g_atfork[i].child();
}

/* ── posix_spawn ── fork()+exec() fallback (b1nix has no spawn syscall). */

enum { SPAWN_OPEN, SPAWN_CLOSE, SPAWN_DUP2, SPAWN_CHDIR };
struct __spawn_action {
  int type;
  int fd;
  int newfd;
  int oflag;
  mode_t mode;
  char *path;
};

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) {
  fa->count = 0;
  fa->cap = 0;
  fa->actions = (struct __spawn_action *)0;
  return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
  if (fa->actions) {
    for (int i = 0; i < fa->count; i++) free(fa->actions[i].path);
    free(fa->actions);
  }
  fa->actions = (struct __spawn_action *)0;
  fa->count = fa->cap = 0;
  return 0;
}

static struct __spawn_action *spawn_action_add(posix_spawn_file_actions_t *fa) {
  if (fa->count >= fa->cap) {
    int ncap = fa->cap ? fa->cap * 2 : 8;
    struct __spawn_action *na =
        (struct __spawn_action *)realloc(fa->actions, (size_t)ncap * sizeof(*na));
    if (!na) return (struct __spawn_action *)0;
    fa->actions = na;
    fa->cap = ncap;
  }
  struct __spawn_action *a = &fa->actions[fa->count++];
  memset(a, 0, sizeof(*a));
  return a;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *path, int oflag, mode_t mode) {
  struct __spawn_action *a = spawn_action_add(fa);
  if (!a) return ENOMEM;
  a->type = SPAWN_OPEN; a->fd = fd; a->oflag = oflag; a->mode = mode;
  a->path = strdup(path);
  if (!a->path) { fa->count--; return ENOMEM; }
  return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
  struct __spawn_action *a = spawn_action_add(fa);
  if (!a) return ENOMEM;
  a->type = SPAWN_CLOSE; a->fd = fd;
  return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd,
                                     int newfd) {
  struct __spawn_action *a = spawn_action_add(fa);
  if (!a) return ENOMEM;
  a->type = SPAWN_DUP2; a->fd = fd; a->newfd = newfd;
  return 0;
}

int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *fa,
                                         const char *path) {
  struct __spawn_action *a = spawn_action_add(fa);
  if (!a) return ENOMEM;
  a->type = SPAWN_CHDIR;
  a->path = strdup(path);
  if (!a->path) { fa->count--; return ENOMEM; }
  return 0;
}

int posix_spawnattr_init(posix_spawnattr_t *attr) {
  memset(attr, 0, sizeof(*attr));
  return 0;
}
int posix_spawnattr_destroy(posix_spawnattr_t *attr) { (void)attr; return 0; }
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags) {
  *flags = attr->flags; return 0;
}
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags) {
  attr->flags = flags; return 0;
}
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup) {
  *pgroup = attr->pgroup; return 0;
}
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup) {
  attr->pgroup = pgroup; return 0;
}
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *sd) {
  *sd = attr->sigdefault; return 0;
}
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *sd) {
  attr->sigdefault = *sd; return 0;
}
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *sm) {
  *sm = attr->sigmask; return 0;
}
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sm) {
  attr->sigmask = *sm; return 0;
}

static int spawn_exec_path(const char *file, char *const argv[],
                           char *const envp[]) {
  /* execve with an explicit envp, searching PATH when `file` has no slash. */
  if (strchr(file, '/')) {
    execve(file, argv, envp);
    return -1;
  }
  const char *path = (const char *)0;
  for (char *const *e = envp; e && *e; e++) {
    if (strncmp(*e, "PATH=", 5) == 0) { path = *e + 5; break; }
  }
  if (!path) path = "/bin:/usr/bin";
  char buf[1024];
  const char *p = path;
  while (*p) {
    const char *colon = strchr(p, ':');
    size_t len = colon ? (size_t)(colon - p) : strlen(p);
    if (len + 1 + strlen(file) + 1 <= sizeof(buf)) {
      memcpy(buf, p, len);
      buf[len] = '/';
      strcpy(buf + len + 1, file);
      execve(buf, argv, envp);
    }
    if (!colon) break;
    p = colon + 1;
  }
  return -1;
}

static int do_spawn(pid_t *pid, const char *path,
                    const posix_spawn_file_actions_t *fa,
                    const posix_spawnattr_t *attrp, char *const argv[],
                    char *const envp[], int use_path) {
  pid_t c = fork();
  if (c < 0) return errno;
  if (c == 0) {
    if (attrp) {
      short f = attrp->flags;
      if (f & POSIX_SPAWN_SETSID) setsid();
      if (f & POSIX_SPAWN_SETPGROUP) setpgid(0, attrp->pgroup);
      if (f & POSIX_SPAWN_SETSIGDEF) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        for (int s = 1; s < 32; s++)
          if (sigismember(&attrp->sigdefault, s)) sigaction(s, &sa, (void *)0);
      }
      if (f & POSIX_SPAWN_SETSIGMASK)
        sigprocmask(SIG_SETMASK, &attrp->sigmask, (void *)0);
    }
    if (fa) {
      for (int i = 0; i < fa->count; i++) {
        struct __spawn_action *a = &fa->actions[i];
        switch (a->type) {
          case SPAWN_OPEN: {
            int ofd = open(a->path, a->oflag, a->mode);
            if (ofd < 0) _exit(127);
            if (ofd != a->fd) { dup2(ofd, a->fd); close(ofd); }
            break;
          }
          case SPAWN_CLOSE: close(a->fd); break;
          case SPAWN_DUP2: dup2(a->fd, a->newfd); break;
          case SPAWN_CHDIR: if (chdir(a->path) != 0) _exit(127); break;
        }
      }
    }
    if (use_path) spawn_exec_path(path, argv, envp);
    else execve(path, argv, envp);
    _exit(127);
  }
  if (pid) *pid = c;
  return 0;
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp, char *const argv[],
                char *const envp[]) {
  return do_spawn(pid, path, file_actions, attrp, argv, envp, 0);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp, char *const argv[],
                 char *const envp[]) {
  return do_spawn(pid, file, file_actions, attrp, argv, envp, 1);
}

/* truncate/fchmodat for libc++'s std::filesystem. */
int truncate(const char *path, off_t length) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  int r = ftruncate(fd, length);
  int e = errno;
  close(fd);
  if (r < 0) errno = e;
  return r;
}

int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
  (void)flags;
  if (dirfd == AT_FDCWD) return chmod(path, (unsigned int)mode);
  errno = ENOSYS;
  return -1;
}
