/*
 * m124_smoke — the modern system calls M124 closes: memfd_secret(2),
 * protection keys and filesystem quotas.
 *
 * Every check runs in a forked child and reports through a pipe what it saw;
 * a marker is printed only after the property was observed, a refusal only
 * with the errno Linux gives.
 *
 *   secret-create          memfd_secret returns a regular file; O_CLOEXEC is
 *                          honoured and any other flag is EINVAL
 *   secret-size-once       ftruncate sets the size once; a second one is EINVAL
 *   secret-no-read-write   read(2) and write(2) on the descriptor are EINVAL
 *   secret-private-refused a MAP_PRIVATE mapping is EINVAL
 *   secret-map-rw          a shared mapping keeps what is written across
 *                          munmap and a second mmap of the same file
 *   secret-past-eof        touching a page past the end raises SIGBUS with
 *                          BUS_ADRERR at that address
 *   secret-fork-shared     a forked child's writes are seen by the parent
 *   secret-syscall-io      the kernel still copies to and from the mapping on
 *                          the owner's behalf (pipe write and read)
 *   secret-foreign-refused /proc/self/mem reads EIO, process_vm_readv EFAULT and
 *                          PTRACE_PEEKDATA EIO on a secret page, while an
 *                          ordinary page next to it reads fine
 *   secret-memlock         the mapping counts against RLIMIT_MEMLOCK for a
 *                          task without CAP_IPC_LOCK (EAGAIN over the limit)
 *   secret-scrubbed        a page freed by one secret file reads as zeros in
 *                          the next, whether its block was kept or released
 *   secret-many            600 pages (more than one hidden 2 MiB block) each
 *                          keep their own contents
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define NR_memfd_secret 447
#define PG 4096
#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

static int g_fail;

static void marker(const char *s) { write(1, s, strlen(s)); }

static void ok(const char *name) {
  char b[128];
  snprintf(b, sizeof(b), "M124-SMOKE: ok %s\n", name);
  marker(b);
}

static void fail(const char *name, const char *why) {
  char b[512];
  snprintf(b, sizeof(b), "M124-SMOKE: FAIL %s %s\n", name, why);
  marker(b);
  g_fail = 1;
}

typedef void (*check_fn)(int report_fd);

static void reportf(int fd, const char *fmt, ...) {
  char b[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  write(fd, b, strlen(b));
}

static void run(const char *name, check_fn fn) {
  int p[2];
  if (pipe(p) != 0) {
    fail(name, "pipe");
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    fail(name, "fork");
    return;
  }
  if (pid == 0) {
    close(p[0]);
    fn(p[1]);
    close(p[1]);
    _exit(0);
  }
  close(p[1]);
  char buf[512];
  size_t n = 0;
  for (;;) {
    ssize_t r = read(p[0], buf + n, sizeof(buf) - 1 - n);
    if (r <= 0)
      break;
    n += (size_t)r;
    if (n == sizeof(buf) - 1)
      break;
  }
  buf[n] = '\0';
  close(p[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  if (n > 0)
    fail(name, buf);
  else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    fail(name, "check process died");
  else
    ok(name);
}

static int secret(int flags) { return (int)syscall(NR_memfd_secret, flags); }

/* A secret file of `pages` pages, mapped shared. */
static char *secret_map(int *fd_out, size_t pages) {
  int fd = secret(0);
  if (fd < 0)
    return 0;
  if (ftruncate(fd, (off_t)(pages * PG)) != 0) {
    close(fd);
    return 0;
  }
  char *p = mmap(0, pages * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    close(fd);
    return 0;
  }
  *fd_out = fd;
  return p;
}

/* ── memfd_secret ─────────────────────────────────────────────────────────── */

static void check_secret_create(int r) {
  int fd = secret(0);
  if (fd < 0) {
    reportf(r, "memfd_secret(0) errno %d", errno);
    return;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 0) {
    reportf(r, "fstat mode 0%o size %lld", st.st_mode, (long long)st.st_size);
    return;
  }
  if (fcntl(fd, F_GETFD) & FD_CLOEXEC) {
    reportf(r, "FD_CLOEXEC set without O_CLOEXEC");
    return;
  }
  int fd2 = secret(O_CLOEXEC);
  if (fd2 < 0 || !(fcntl(fd2, F_GETFD) & FD_CLOEXEC)) {
    reportf(r, "O_CLOEXEC not honoured (fd %d errno %d)", fd2, errno);
    return;
  }
  errno = 0;
  if (secret(O_NONBLOCK) != -1 || errno != EINVAL) {
    reportf(r, "O_NONBLOCK accepted (errno %d)", errno);
    return;
  }
}

static void check_secret_size_once(int r) {
  int fd = secret(0);
  if (fd < 0 || ftruncate(fd, 3 * PG) != 0) {
    reportf(r, "first ftruncate errno %d", errno);
    return;
  }
  errno = 0;
  if (ftruncate(fd, 2 * PG) != -1 || errno != EINVAL) {
    reportf(r, "second ftruncate not EINVAL (errno %d)", errno);
    return;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size != 3 * PG)
    reportf(r, "size %lld after refusal", (long long)st.st_size);
}

static void check_secret_no_read_write(int r) {
  int fd = secret(0);
  char c = 'x';
  if (fd < 0 || ftruncate(fd, PG) != 0) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  errno = 0;
  if (read(fd, &c, 1) != -1 || errno != EINVAL) {
    reportf(r, "read not EINVAL (errno %d)", errno);
    return;
  }
  errno = 0;
  if (write(fd, &c, 1) != -1 || errno != EINVAL)
    reportf(r, "write not EINVAL (errno %d)", errno);
}

static void check_secret_private_refused(int r) {
  int fd = secret(0);
  if (fd < 0 || ftruncate(fd, PG) != 0) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  errno = 0;
  void *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p != MAP_FAILED || errno != EINVAL)
    reportf(r, "MAP_PRIVATE gave %p errno %d", p, errno);
}

static void check_secret_map_rw(int r) {
  int fd;
  char *p = secret_map(&fd, 3);
  if (!p) {
    reportf(r, "map errno %d", errno);
    return;
  }
  for (int i = 0; i < 3 * PG; i++)
    p[i] = (char)(i * 7 + 3);
  munmap(p, 3 * PG);
  p = mmap(0, 3 * PG, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    reportf(r, "second mmap errno %d", errno);
    return;
  }
  for (int i = 0; i < 3 * PG; i++) {
    if (p[i] != (char)(i * 7 + 3)) {
      reportf(r, "byte %d is %d after remap", i, p[i]);
      return;
    }
  }
}

static sigjmp_buf g_jmp;
static volatile int g_sig, g_code;
static void *volatile g_addr;

static void on_fault(int sig, siginfo_t *si, void *uc) {
  (void)uc;
  g_sig = sig;
  g_code = si->si_code;
  g_addr = si->si_addr;
  siglongjmp(g_jmp, 1);
}

static void check_secret_past_eof(int r) {
  int fd = secret(0);
  if (fd < 0 || ftruncate(fd, 2 * PG) != 0) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  char *p = mmap(0, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    reportf(r, "mmap past the end errno %d", errno);
    return;
  }
  p[PG] = 1; /* inside */
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGBUS, &sa, 0);
  sigaction(SIGSEGV, &sa, 0);
  if (sigsetjmp(g_jmp, 1) == 0) {
    p[2 * PG + 5] = 1;
    reportf(r, "store past the end succeeded");
    return;
  }
  if (g_sig != SIGBUS || g_code != BUS_ADRERR || g_addr != p + 2 * PG + 5)
    reportf(r, "signal %d code %d addr %p (want SIGBUS/BUS_ADRERR at %p)",
            g_sig, g_code, g_addr, (void *)(p + 2 * PG + 5));
}

static void check_secret_fork_shared(int r) {
  int fd;
  char *p = secret_map(&fd, 1);
  if (!p) {
    reportf(r, "map errno %d", errno);
    return;
  }
  p[10] = 'a';
  pid_t c = fork();
  if (c == 0) {
    p[10] = 'b';
    p[11] = 'c';
    _exit(p[10] == 'b' ? 0 : 1);
  }
  int st;
  waitpid(c, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || p[10] != 'b' || p[11] != 'c')
    reportf(r, "parent reads %c%c after child's write", p[10], p[11]);
}

static void check_secret_syscall_io(int r) {
  int fd;
  char *p = secret_map(&fd, 2);
  int pp[2];
  if (!p || pipe(pp) != 0) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  /* Straddle the page boundary, first page never touched by the process. */
  memcpy(p + PG - 3, "secret", 6);
  if (write(pp[1], p + PG - 3, 6) != 6) {
    reportf(r, "write from mapping errno %d", errno);
    return;
  }
  char out[8] = {0};
  if (read(pp[0], out, 6) != 6 || memcmp(out, "secret", 6) != 0) {
    reportf(r, "pipe carried '%.6s'", out);
    return;
  }
  if (write(pp[1], "kernel", 6) != 6 || read(pp[0], p + 20, 6) != 6 ||
      memcmp(p + 20, "kernel", 6) != 0)
    reportf(r, "read into mapping errno %d", errno);
}

static void check_secret_foreign_refused(int r) {
  int fd;
  char *p = secret_map(&fd, 1);
  char plain[16] = "ordinary";
  if (!p) {
    reportf(r, "map errno %d", errno);
    return;
  }
  memcpy(p, "hidden!", 8);

  int mem = open("/proc/self/mem", O_RDONLY);
  char b[8];
  if (mem < 0) {
    reportf(r, "open /proc/self/mem errno %d", errno);
    return;
  }
  if (pread(mem, b, 8, (off_t)(uintptr_t)plain) != 8 ||
      memcmp(b, "ordinary", 8) != 0) {
    reportf(r, "/proc/self/mem cannot read an ordinary page (errno %d)", errno);
    return;
  }
  errno = 0;
  if (pread(mem, b, 8, (off_t)(uintptr_t)p) != -1 || errno != EIO) {
    reportf(r, "/proc/self/mem read of secret page not EIO (errno %d)", errno);
    return;
  }

  struct iovec local = {b, 8}, remote = {p, 8};
  errno = 0;
  if (syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) != -1 ||
      errno != EFAULT) {
    reportf(r, "process_vm_readv not EFAULT (errno %d)", errno);
    return;
  }

  /* A tracer: the child stops itself, the parent peeks. */
  pid_t c = fork();
  if (c == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    raise(SIGSTOP);
    _exit(0);
  }
  int st;
  waitpid(c, &st, 0);
  errno = 0;
  long w = ptrace(PTRACE_PEEKDATA, c, p, 0);
  int peek_errno = errno;
  errno = 0;
  long wp = ptrace(PTRACE_PEEKDATA, c, plain, 0);
  int plain_errno = errno;
  kill(c, SIGKILL);
  waitpid(c, &st, 0);
  if (peek_errno != EIO)
    reportf(r, "PTRACE_PEEKDATA on secret page gave %lx errno %d", w,
            peek_errno);
  else if (plain_errno != 0 || memcmp(&wp, "ordinary", 8) != 0)
    reportf(r, "PTRACE_PEEKDATA on an ordinary page errno %d", plain_errno);
}

static void check_secret_memlock(int r) {
  struct rlimit lim = {PG, PG};
  if (setrlimit(RLIMIT_MEMLOCK, &lim) != 0 || setgid(UNPRIV_GID) != 0 ||
      setuid(UNPRIV_UID) != 0) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  int fd = secret(0);
  if (fd < 0 || ftruncate(fd, 2 * PG) != 0) {
    reportf(r, "create errno %d", errno);
    return;
  }
  errno = 0;
  void *big = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (big != MAP_FAILED || errno != EAGAIN) {
    reportf(r, "2 pages over a 1-page limit: %p errno %d", big, errno);
    return;
  }
  char *one = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (one == MAP_FAILED) {
    reportf(r, "1 page within the limit errno %d", errno);
    return;
  }
  one[0] = 1;
  errno = 0;
  void *more = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PG);
  if (more != MAP_FAILED || errno != EAGAIN)
    reportf(r, "a second page beside the first: %p errno %d", more, errno);
}

static int all_zero(const char *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i])
      return 0;
  return 1;
}

static void check_secret_scrubbed(int r) {
  /* A page kept alive holds its block, so a page freed beside it is reused
   * from that same block. */
  int keep_fd, a_fd, b_fd;
  char *keep = secret_map(&keep_fd, 1);
  char *a = secret_map(&a_fd, 8);
  if (!keep || !a) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  keep[0] = 1;
  memset(a, 0x5a, 8 * PG);
  munmap(a, 8 * PG);
  close(a_fd);
  char *b = secret_map(&b_fd, 8);
  if (!b) {
    reportf(r, "second file errno %d", errno);
    return;
  }
  if (!all_zero(b, 8 * PG)) {
    reportf(r, "reused pages still hold the old contents");
    return;
  }
  /* And with nothing left alive, the block goes back and comes out clean. */
  munmap(b, 8 * PG);
  close(b_fd);
  munmap(keep, PG);
  close(keep_fd);
  char *c = secret_map(&b_fd, 8);
  if (!c || !all_zero(c, 8 * PG))
    reportf(r, "fresh block not zero (map %p)", (void *)c);
}

static void check_secret_many(int r) {
  enum { N = 600 };
  int fd;
  char *p = secret_map(&fd, N);
  if (!p) {
    reportf(r, "map errno %d", errno);
    return;
  }
  for (int i = 0; i < N; i++)
    memcpy(p + (size_t)i * PG + 100, &i, sizeof(i));
  for (int i = 0; i < N; i++) {
    int v;
    memcpy(&v, p + (size_t)i * PG + 100, sizeof(v));
    if (v != i) {
      reportf(r, "page %d holds %d", i, v);
      return;
    }
  }
}

int main(void) {
  marker("M124-SMOKE: start\n");
  run("secret-create", check_secret_create);
  run("secret-size-once", check_secret_size_once);
  run("secret-no-read-write", check_secret_no_read_write);
  run("secret-private-refused", check_secret_private_refused);
  run("secret-map-rw", check_secret_map_rw);
  run("secret-past-eof", check_secret_past_eof);
  run("secret-fork-shared", check_secret_fork_shared);
  run("secret-syscall-io", check_secret_syscall_io);
  run("secret-foreign-refused", check_secret_foreign_refused);
  run("secret-memlock", check_secret_memlock);
  run("secret-scrubbed", check_secret_scrubbed);
  run("secret-many", check_secret_many);
  marker(g_fail ? "M124-SMOKE: done with failures\n" : "M124-SMOKE: done\n");
  return 0;
}
