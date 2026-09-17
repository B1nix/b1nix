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
 *
 * Protection keys, on a CPU without them (the ordinary lanes' KVM CPUs):
 *   pkey-absent            pkey_alloc is ENOSPC, pkey_free EINVAL,
 *                          pkey_mprotect with key -1 is mprotect and with any
 *                          other key EINVAL, and nothing claims pku in cpuinfo
 * and on one with them (the pku lane, TCG -cpu max):
 *   pkey-cpuinfo           /proc/cpuinfo lists pku and ospke
 *   pkey-alloc             keys 1..15 in order, then ENOSPC; a freed key is
 *                          handed out again; bad flags, rights and frees EINVAL
 *   pkey-rights            pkey_alloc's rights land in the caller's PKRU
 *   pkey-access-disable    a page under a key its PKRU access-disables faults
 *                          on read with SIGSEGV/SEGV_PKUERR naming the key
 *   pkey-write-disable     write-disable lets reads through and faults writes
 *   pkey-mprotect-keeps    plain mprotect keeps a mapping's key; pkey_mprotect
 *                          of an unallocated key is EINVAL
 *   pkey-kernel-copy       write(2) from and read(2) into a key-refused page
 *                          are EFAULT, and work once the rights allow them
 *   pkey-threads           a new thread starts with its creator's PKRU, and
 *                          rights one thread changes are its own
 *   pkey-signal            a handler runs with the initial rights, and the
 *                          interrupted rights are back after it returns
 *   pkey-fork-exec         a fork child keeps the keys and rights; an exec'd
 *                          program has no keys and the initial PKRU
 *   pkey-exec-only         mprotect(PROT_EXEC) alone makes code callable but
 *                          unreadable (SEGV_PKUERR on a read)
 *
 * Quotas, on an ext4 made by the distribution's mkfs.ext4 -O quota and mounted
 * from a loop device (one sequence: each step builds on the last):
 *   quota-format           Q_GETFMT is QFMT_VFS_V1, Q_GETINFO reports the
 *                          quota as a system file, Q_XGETQSTAT accounting on
 *   quota-usage            a user's files are charged to that user: space and
 *                          inodes in Q_GETQUOTA
 *   quota-enforce          Q_SETQUOTA limits plus Q_QUOTAON: a write past the
 *                          hard limit fails with EDQUOT, one within it works
 *   quota-next             Q_GETNEXTQUOTA finds the next id with usage
 *   quota-permissions      a user reads its own usage but not another's
 *                          (EPERM), and cannot set limits (EPERM)
 *   quota-fd               quotactl_fd answers through a descriptor, and a
 *                          filesystem without quotas is ENOSYS
 *   quota-off              Q_QUOTAOFF lifts enforcement, accounting stays
 *   quota-tools            quota-tools' setquota sets a limit Q_GETQUOTA reads
 *                          back, and repquota lists the user
 *   quota-persist          limits survive umount and mount, and e2fsck -fn
 *                          finds the quota files consistent with the usage
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/quota.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <pthread.h>
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


/* ── protection keys ──────────────────────────────────────────────────────── */

#define PKRU_INIT 0x55555554u
#define PKEY_DISABLE_ACCESS 0x1
#define PKEY_DISABLE_WRITE 0x2

static int pkey_alloc_(unsigned long flags, unsigned long rights) {
  return (int)syscall(SYS_pkey_alloc, flags, rights);
}
static int pkey_free_(int k) { return (int)syscall(SYS_pkey_free, k); }
static int pkey_mprotect_(void *a, size_t n, int prot, int k) {
  return (int)syscall(SYS_pkey_mprotect, a, n, prot, k);
}

/* The whole of a /proc file: a read may return less than the file holds. */
static ssize_t read_all(const char *path, char *b, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  size_t n = 0;
  for (;;) {
    ssize_t got = read(fd, b + n, cap - 1 - n);
    if (got <= 0)
      break;
    n += (size_t)got;
    if (n == cap - 1)
      break;
  }
  close(fd);
  b[n] = 0;
  return (ssize_t)n;
}

static int cpu_has_pkeys(void) {
  char b[16384];
  return read_all("/proc/cpuinfo", b, sizeof(b)) > 0 && strstr(b, " ospke") != 0;
}

#if defined(__x86_64__)
static unsigned rdpkru_(void) {
  unsigned a, d;
  __asm__ volatile("rdpkru" : "=a"(a), "=d"(d) : "c"(0));
  (void)d;
  return a;
}
static void wrpkru_(unsigned v) { __asm__ volatile("wrpkru" : : "a"(v), "c"(0), "d"(0)); }
#else
static unsigned rdpkru_(void) { return 0; }
static void wrpkru_(unsigned v) { (void)v; }
#endif

static void set_rights(int k, unsigned rights) {
  unsigned v = rdpkru_();
  v &= ~(3u << (2 * k));
  wrpkru_(v | (rights << (2 * k)));
}

static void check_pkey_absent(int r) {
  errno = 0;
  if (pkey_alloc_(0, 0) != -1 || errno != ENOSPC) {
    reportf(r, "pkey_alloc not ENOSPC (errno %d)", errno);
    return;
  }
  errno = 0;
  if (pkey_free_(1) != -1 || errno != EINVAL) {
    reportf(r, "pkey_free(1) not EINVAL (errno %d)", errno);
    return;
  }
  char *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED || pkey_mprotect_(p, PG, PROT_READ, -1) != 0) {
    reportf(r, "pkey_mprotect(-1) errno %d", errno);
    return;
  }
  errno = 0;
  if (pkey_mprotect_(p, PG, PROT_READ, 1) != -1 || errno != EINVAL)
    reportf(r, "pkey_mprotect(1) not EINVAL (errno %d)", errno);
}

static void check_pkey_cpuinfo(int r) {
  char b[16384];
  if (read_all("/proc/cpuinfo", b, sizeof(b)) <= 0) {
    reportf(r, "read /proc/cpuinfo errno %d", errno);
    return;
  }
  if (!strstr(b, " pku ") || !strstr(b, " ospke"))
    reportf(r, "flags lack pku/ospke");
}

static void check_pkey_alloc(int r) {
  for (int want = 1; want < 16; want++) {
    int k = pkey_alloc_(0, 0);
    if (k != want) {
      reportf(r, "allocation %d returned %d errno %d", want, k, errno);
      return;
    }
  }
  errno = 0;
  if (pkey_alloc_(0, 0) != -1 || errno != ENOSPC) {
    reportf(r, "16th key not ENOSPC (errno %d)", errno);
    return;
  }
  if (pkey_free_(7) != 0 || pkey_alloc_(0, 0) != 7) {
    reportf(r, "freed key 7 not handed out again");
    return;
  }
  if (pkey_free_(9) != 0) {
    reportf(r, "pkey_free(9) errno %d", errno);
    return;
  }
  errno = 0;
  if (pkey_free_(9) != -1 || errno != EINVAL) {
    reportf(r, "double pkey_free not EINVAL (errno %d)", errno);
    return;
  }
  errno = 0;
  if (pkey_free_(16) != -1 || errno != EINVAL) {
    reportf(r, "pkey_free(16) not EINVAL (errno %d)", errno);
    return;
  }
  errno = 0;
  if (pkey_alloc_(1, 0) != -1 || errno != EINVAL) {
    reportf(r, "flags accepted (errno %d)", errno);
    return;
  }
  errno = 0;
  if (pkey_alloc_(0, 4) != -1 || errno != EINVAL)
    reportf(r, "rights 4 accepted (errno %d)", errno);
}

static void check_pkey_rights(int r) {
  int a = pkey_alloc_(0, PKEY_DISABLE_ACCESS);
  int w = pkey_alloc_(0, PKEY_DISABLE_WRITE);
  int n = pkey_alloc_(0, 0);
  unsigned v = rdpkru_();
  if (a < 0 || w < 0 || n < 0) {
    reportf(r, "alloc errno %d", errno);
    return;
  }
  if (((v >> (2 * a)) & 3) != 1 || ((v >> (2 * w)) & 3) != 2 ||
      ((v >> (2 * n)) & 3) != 0)
    reportf(r, "PKRU %08x for keys %d/%d/%d", v, a, w, n);
}

static char *keyed_page(int *key_out, int rights) {
  int k = pkey_alloc_(0, 0);
  char *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (k < 0 || p == MAP_FAILED)
    return 0;
  p[0] = 'k'; /* resident before the key: both kinds of entry get covered */
  if (pkey_mprotect_(p, PG, PROT_READ | PROT_WRITE, k) != 0)
    return 0;
  set_rights(k, (unsigned)rights);
  *key_out = k;
  return p;
}

static int fault_with(void (*fn)(char *), char *p) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, 0);
  g_sig = 0;
  if (sigsetjmp(g_jmp, 1) == 0) {
    fn(p);
    return 0;
  }
  return 1;
}

static void touch_read(char *p) { volatile char c = p[1]; (void)c; }
static void touch_write(char *p) { p[2] = 'w'; }

/* si_pkey: _sigfault._addr_pkey._pkey, 32 bytes into siginfo_t. */
static volatile unsigned g_pkey_seen;
static void on_pkey_fault(int sig, siginfo_t *si, void *uc) {
  (void)uc;
  g_sig = sig;
  g_code = si->si_code;
  g_addr = si->si_addr;
  memcpy((void *)&g_pkey_seen, (char *)si + 32, sizeof(unsigned));
  siglongjmp(g_jmp, 1);
}

static int keyed_fault(void (*fn)(char *), char *p) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_pkey_fault;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, 0);
  g_sig = 0;
  if (sigsetjmp(g_jmp, 1) == 0) {
    fn(p);
    return 0;
  }
  return 1;
}

#define SEGV_PKUERR_ 4

static void check_pkey_access_disable(int r) {
  int k;
  char *p = keyed_page(&k, PKEY_DISABLE_ACCESS);
  if (!p) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  if (!keyed_fault(touch_read, p) || g_sig != SIGSEGV || g_code != SEGV_PKUERR_ ||
      g_addr != p + 1 || g_pkey_seen != (unsigned)k) {
    reportf(r, "read: signal %d code %d addr %p pkey %u (want key %d at %p)",
            g_sig, g_code, g_addr, g_pkey_seen, k, (void *)(p + 1));
    return;
  }
  set_rights(k, 0);
  if (fault_with(touch_read, p) || p[0] != 'k')
    reportf(r, "read with the rights cleared faulted");
}

static void check_pkey_write_disable(int r) {
  int k;
  char *p = keyed_page(&k, PKEY_DISABLE_WRITE);
  if (!p) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  if (fault_with(touch_read, p)) {
    reportf(r, "read under write-disable faulted (code %d)", g_code);
    return;
  }
  if (!keyed_fault(touch_write, p) || g_code != SEGV_PKUERR_ ||
      g_pkey_seen != (unsigned)k) {
    reportf(r, "write: faulted=%d code %d pkey %u", g_sig, g_code, g_pkey_seen);
    return;
  }
  set_rights(k, 0);
  if (fault_with(touch_write, p) || p[2] != 'w')
    reportf(r, "write with the rights cleared failed");
}

static void check_pkey_mprotect_keeps(int r) {
  int k;
  char *p = keyed_page(&k, 0);
  if (!p) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  /* A page never touched before the key, too. */
  char *q = mmap(0, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (q == MAP_FAILED || pkey_mprotect_(q, 4 * PG, PROT_READ | PROT_WRITE, k) ||
      mprotect(p, PG, PROT_READ) || mprotect(q, 4 * PG, PROT_READ | PROT_WRITE)) {
    reportf(r, "mprotect errno %d", errno);
    return;
  }
  set_rights(k, PKEY_DISABLE_ACCESS);
  if (!keyed_fault(touch_read, p) || g_pkey_seen != (unsigned)k) {
    reportf(r, "mprotect dropped the key of a resident page");
    return;
  }
  if (!keyed_fault(touch_read, q + 3 * PG) || g_code != SEGV_PKUERR_ ||
      g_pkey_seen != (unsigned)k) {
    reportf(r, "a page first touched after the key lacks it (code %d)", g_code);
    return;
  }
  set_rights(k, 0);
  errno = 0;
  if (pkey_mprotect_(q, PG, PROT_READ, 14) != -1 || errno != EINVAL)
    reportf(r, "unallocated key 14 accepted (errno %d)", errno);
}

static void check_pkey_kernel_copy(int r) {
  int k;
  char *p = keyed_page(&k, PKEY_DISABLE_ACCESS);
  int pp[2];
  if (!p || pipe(pp) != 0) {
    reportf(r, "setup errno %d", errno);
    return;
  }
  errno = 0;
  if (write(pp[1], p, 4) != -1 || errno != EFAULT) {
    reportf(r, "write from a refused page not EFAULT (errno %d)", errno);
    return;
  }
  if (write(pp[1], "abcd", 4) != 4) {
    reportf(r, "pipe write errno %d", errno);
    return;
  }
  errno = 0;
  if (read(pp[0], p + 8, 4) != -1 || errno != EFAULT) {
    reportf(r, "read into a refused page not EFAULT (errno %d)", errno);
    return;
  }
  set_rights(k, 0);
  /* A read that failed may or may not have consumed the bytes; offer them
   * again so either way the next four read "abcd". */
  if (write(pp[1], "abcd", 4) != 4 || read(pp[0], p + 8, 4) != 4 ||
      memcmp(p + 8, "abcd", 4) != 0 || write(pp[1], p, 1) != 1)
    reportf(r, "copies with the rights cleared failed (errno %d)", errno);
}

static volatile unsigned g_thread_pkru_start, g_thread_pkru_after;
static void *pkey_thread(void *arg) {
  g_thread_pkru_start = rdpkru_();
  set_rights((int)(long)arg, PKEY_DISABLE_WRITE);
  g_thread_pkru_after = rdpkru_();
  return 0;
}

static void check_pkey_threads(int r) {
  int k = pkey_alloc_(0, PKEY_DISABLE_ACCESS);
  if (k < 0) {
    reportf(r, "alloc errno %d", errno);
    return;
  }
  unsigned mine = rdpkru_();
  pthread_t t;
  if (pthread_create(&t, 0, pkey_thread, (void *)(long)k) != 0) {
    reportf(r, "pthread_create failed");
    return;
  }
  pthread_join(t, 0);
  if (g_thread_pkru_start != mine) {
    reportf(r, "thread started with %08x, creator had %08x", g_thread_pkru_start,
            mine);
    return;
  }
  if (((g_thread_pkru_after >> (2 * k)) & 3) != 2 || rdpkru_() != mine)
    reportf(r, "thread's change leaked (thread %08x, creator had %08x, now %08x)",
            g_thread_pkru_after, mine, rdpkru_());
}

static volatile unsigned g_handler_pkru;
static void on_usr1(int sig) {
  (void)sig;
  g_handler_pkru = rdpkru_();
}

static void check_pkey_signal(int r) {
  int k = pkey_alloc_(0, 0);
  if (k < 0) {
    reportf(r, "alloc errno %d", errno);
    return;
  }
  set_rights(k, PKEY_DISABLE_WRITE);
  unsigned before = rdpkru_();
  signal(SIGUSR1, on_usr1);
  raise(SIGUSR1);
  if (g_handler_pkru != PKRU_INIT) {
    reportf(r, "handler ran with %08x", g_handler_pkru);
    return;
  }
  if (rdpkru_() != before)
    reportf(r, "after the handler %08x, before %08x", rdpkru_(), before);
}

static void check_pkey_fork_exec(int r) {
  int k = pkey_alloc_(0, PKEY_DISABLE_WRITE);
  if (k < 0) {
    reportf(r, "alloc errno %d", errno);
    return;
  }
  unsigned mine = rdpkru_();
  pid_t c = fork();
  if (c == 0) {
    int ok_rights = rdpkru_() == mine;
    int ok_key = pkey_free_(k) == 0;
    _exit(ok_rights && ok_key ? 0 : 1);
  }
  int st;
  waitpid(c, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    reportf(r, "fork child lost the key or the rights");
    return;
  }
  char karg[8];
  snprintf(karg, sizeof(karg), "%d", k);
  c = fork();
  if (c == 0) {
    execl("/proc/self/exe", "m124_smoke", "--pkey-exec-probe", karg, (char *)0);
    _exit(2);
  }
  waitpid(c, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
    reportf(r, "exec'd program: status %d (1 = key or rights survived exec)",
            WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

/* In the exec'd image: no key but 0 is allocated, and PKRU is the initial one. */
static int pkey_exec_probe(const char *karg) {
  int k = atoi(karg);
  errno = 0;
  int freed = pkey_free_(k);
  return (freed == -1 && errno == EINVAL && rdpkru_() == PKRU_INIT) ? 0 : 1;
}

static void check_pkey_exec_only(int r) {
  char *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    reportf(r, "mmap errno %d", errno);
    return;
  }
  /* mov $42, %eax; ret */
  static const unsigned char code[] = {0xb8, 42, 0, 0, 0, 0xc3};
  memcpy(p, code, sizeof(code));
  if (mprotect(p, PG, PROT_EXEC) != 0) {
    reportf(r, "mprotect(PROT_EXEC) errno %d", errno);
    return;
  }
  int (*fn)(void) = (int (*)(void))(void *)p;
  if (fn() != 42) {
    reportf(r, "execute-only code did not run");
    return;
  }
  if (!keyed_fault(touch_read, p) || g_code != SEGV_PKUERR_) {
    reportf(r, "read of execute-only page: faulted %d code %d", g_sig, g_code);
    return;
  }
  /* And back: readable again once it is not execute-only. */
  if (mprotect(p, PG, PROT_READ | PROT_EXEC) != 0 || p[0] != (char)0xb8)
    reportf(r, "PROT_READ|PROT_EXEC did not make it readable");
}

/* ── quotas ───────────────────────────────────────────────────────────────── */

#define NR_quotactl_fd 443
#ifndef Q_GETNEXTQUOTA
#define Q_GETNEXTQUOTA 0x800009
#endif
#define QFMT_VFS_V1_ 4
#define DQF_SYS_FILE_ 0x10000
#define Q_XGETQSTAT_ ((('X' << 8) + 5) << 0)
#define FS_QUOTA_UDQ_ACCT_ 1
#define FS_QUOTA_UDQ_ENFD_ 2
#define LOOP_SET_FD_ 0x4C00
#define LOOP_CLR_FD_ 0x4C01
#define LOOP_CTL_GET_FREE_ 0x4C82
#define QIMG "/tmp/m124-quota.img"
#define QMNT "/tmp/m124-quota"
#define QUSER 1000

struct fs_qfilestat_ {
  uint64_t qfs_ino, qfs_nblks;
  uint32_t qfs_nextents;
};
struct fs_quota_stat_ {
  int8_t qs_version;
  uint16_t qs_flags;
  int8_t qs_pad;
  struct fs_qfilestat_ qs_uquota, qs_gquota;
  uint32_t qs_incoredqs;
  int32_t qs_btimelimit, qs_itimelimit, qs_rtbtimelimit;
  uint16_t qs_bwarnlimit, qs_iwarnlimit;
};
struct if_nextdqblk_ {
  uint64_t dqb_bhardlimit, dqb_bsoftlimit, dqb_curspace, dqb_ihardlimit,
      dqb_isoftlimit, dqb_curinodes, dqb_btime, dqb_itime;
  uint32_t dqb_valid, dqb_id;
};

static char g_qdev[32];

static int sh(const char *cmd) {
  int st = system(cmd);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int quota_mount(void) {
  return mount(g_qdev, QMNT, "ext4", 0, 0);
}

/* Build the filesystem with the distribution's tools and attach it. */
static int quota_setup(char *why, size_t n) {
  unlink(QIMG);
  mkdir(QMNT, 0755);
  int fd = open(QIMG, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0 || ftruncate(fd, 32 << 20) != 0) {
    snprintf(why, n, "image errno %d", errno);
    return -1;
  }
  close(fd);
  if (sh("mkfs.ext4 -q -F -O quota -E quotatype=usrquota:grpquota " QIMG
         " >/dev/null 2>&1") != 0) {
    snprintf(why, n, "mkfs.ext4 -O quota failed");
    return -1;
  }
  int ctl = open("/dev/loop-control", O_RDWR);
  int idx = ctl >= 0 ? ioctl(ctl, LOOP_CTL_GET_FREE_, 0) : -1;
  if (ctl >= 0)
    close(ctl);
  snprintf(g_qdev, sizeof(g_qdev), "/dev/loop%d", idx);
  int lo = idx >= 0 ? open(g_qdev, O_RDWR) : -1;
  int img = open(QIMG, O_RDWR);
  if (lo < 0 || img < 0 || ioctl(lo, LOOP_SET_FD_, img) != 0) {
    snprintf(why, n, "loop attach errno %d", errno);
    return -1;
  }
  close(img);
  close(lo);
  if (quota_mount() != 0) {
    snprintf(why, n, "mount %s errno %d", g_qdev, errno);
    return -1;
  }
  if (mkdir(QMNT "/u", 0755) != 0 || chown(QMNT "/u", QUSER, QUSER) != 0) {
    snprintf(why, n, "user directory errno %d", errno);
    return -1;
  }
  return 0;
}

static void quota_teardown(void) {
  umount(QMNT);
  int lo = open(g_qdev, O_RDWR);
  if (lo >= 0) {
    ioctl(lo, LOOP_CLR_FD_, 0);
    close(lo);
  }
  unlink(QIMG);
}

/* Write `kb` KiB as the quota user into `name`; returns 0, or the errno. */
static int write_as_user(const char *name, int kb) {
  pid_t c = fork();
  if (c == 0) {
    if (setgid(QUSER) || setuid(QUSER))
      _exit(200);
    int fd = open(name, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
      _exit(errno & 0xff);
    char buf[1024];
    memset(buf, 'q', sizeof(buf));
    for (int i = 0; i < kb; i++) {
      if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
        _exit(errno ? errno & 0xff : 201);
    }
    if (fsync(fd) != 0)
      _exit(errno & 0xff);
    _exit(0);
  }
  int st;
  waitpid(c, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int getq(int id, struct dqblk *d) {
  memset(d, 0, sizeof(*d));
  return quotactl(QCMD(Q_GETQUOTA, USRQUOTA), g_qdev, id, (caddr_t)d);
}

/* Run fn as the quota user; it returns 0 or a small failure code. */
static int as_user(int (*fn)(void)) {
  pid_t c = fork();
  if (c == 0) {
    if (setgid(QUSER) || setuid(QUSER))
      _exit(200);
    _exit(fn());
  }
  int st;
  waitpid(c, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int user_quota_rights(void) {
  struct dqblk d;
  if (quotactl(QCMD(Q_GETQUOTA, USRQUOTA), g_qdev, QUSER, (caddr_t)&d) != 0)
    return 1;
  errno = 0;
  if (quotactl(QCMD(Q_GETQUOTA, USRQUOTA), g_qdev, 0, (caddr_t)&d) != -1 ||
      errno != EPERM)
    return 2;
  d.dqb_valid = QIF_BLIMITS;
  errno = 0;
  if (quotactl(QCMD(Q_SETQUOTA, USRQUOTA), g_qdev, QUSER, (caddr_t)&d) != -1 ||
      errno != EPERM)
    return 3;
  return 0;
}

static void quota_suite(void) {
  char why[200];
  static const char *const names[] = {
      "quota-format", "quota-usage", "quota-enforce", "quota-next",
      "quota-permissions", "quota-fd", "quota-off", "quota-tools",
      "quota-persist"};
  enum { N = sizeof(names) / sizeof(names[0]) };
  int i = 0;

  if (quota_setup(why, sizeof(why)) != 0) {
    for (; i < N; i++)
      fail(names[i], why);
    quota_teardown();
    return;
  }
#define STEP_FAIL(...)                                                         \
  do {                                                                         \
    snprintf(why, sizeof(why), __VA_ARGS__);                                   \
    for (; i < N; i++)                                                         \
      fail(names[i], why);                                                     \
    quota_teardown();                                                          \
    return;                                                                    \
  } while (0)

  /* quota-format */
  {
    uint32_t fmt = 0;
    struct dqinfo info;
    struct fs_quota_stat_ qs;
    if (quotactl(QCMD(Q_GETFMT, USRQUOTA), g_qdev, 0, (caddr_t)&fmt) != 0 ||
        fmt != QFMT_VFS_V1_)
      STEP_FAIL("Q_GETFMT %u errno %d", fmt, errno);
    if (quotactl(QCMD(Q_GETINFO, USRQUOTA), g_qdev, 0, (caddr_t)&info) != 0 ||
        !(info.dqi_flags & DQF_SYS_FILE_))
      STEP_FAIL("Q_GETINFO flags %x errno %d", info.dqi_flags, errno);
    memset(&qs, 0, sizeof(qs));
    if (quotactl(QCMD(Q_XGETQSTAT_, USRQUOTA), g_qdev, 0, (caddr_t)&qs) != 0 ||
        !(qs.qs_flags & FS_QUOTA_UDQ_ACCT_) || (qs.qs_flags & FS_QUOTA_UDQ_ENFD_))
      STEP_FAIL("Q_XGETQSTAT flags %x errno %d", qs.qs_flags, errno);
    ok(names[i++]);
  }

  /* quota-usage */
  {
    struct dqblk d;
    int w = write_as_user(QMNT "/u/a", 64);
    if (w != 0)
      STEP_FAIL("write as user %d: %d", QUSER, w);
    if (getq(QUSER, &d) != 0 || d.dqb_curspace < 64 * 1024 || d.dqb_curinodes != 2)
      STEP_FAIL("usage space %llu inodes %llu errno %d",
                (unsigned long long)d.dqb_curspace,
                (unsigned long long)d.dqb_curinodes, errno);
    ok(names[i++]);
  }

  /* quota-enforce */
  {
    struct dqblk d;
    getq(QUSER, &d);
    d.dqb_bhardlimit = 256; /* 1 KiB blocks */
    d.dqb_bsoftlimit = 0;
    d.dqb_valid = QIF_BLIMITS;
    if (quotactl(QCMD(Q_SETQUOTA, USRQUOTA), g_qdev, QUSER, (caddr_t)&d) != 0)
      STEP_FAIL("Q_SETQUOTA errno %d", errno);
    if (quotactl(QCMD(Q_QUOTAON, USRQUOTA), g_qdev, QFMT_VFS_V1_, 0) != 0)
      STEP_FAIL("Q_QUOTAON errno %d", errno);
    int within = write_as_user(QMNT "/u/b", 32);
    int beyond = write_as_user(QMNT "/u/c", 512);
    if (within != 0 || beyond != EDQUOT)
      STEP_FAIL("within the limit %d, beyond it %d (want 0 and EDQUOT)", within,
                beyond);
    if (getq(QUSER, &d) != 0 || d.dqb_bhardlimit != 256 ||
        d.dqb_curspace > 256 * 1024)
      STEP_FAIL("after: limit %llu space %llu",
                (unsigned long long)d.dqb_bhardlimit,
                (unsigned long long)d.dqb_curspace);
    ok(names[i++]);
  }

  /* quota-next */
  {
    struct if_nextdqblk_ nd;
    memset(&nd, 0, sizeof(nd));
    if (quotactl(QCMD(Q_GETNEXTQUOTA, USRQUOTA), g_qdev, 1, (caddr_t)&nd) != 0 ||
        nd.dqb_id != QUSER || nd.dqb_bhardlimit != 256)
      STEP_FAIL("Q_GETNEXTQUOTA from 1: id %u limit %llu errno %d", nd.dqb_id,
                (unsigned long long)nd.dqb_bhardlimit, errno);
    ok(names[i++]);
  }

  /* quota-permissions */
  {
    int r = as_user(user_quota_rights);
    if (r != 0)
      STEP_FAIL("as the user: step %d", r);
    ok(names[i++]);
  }

  /* quota-fd */
  {
    struct dqblk d;
    int dfd = open(QMNT, O_RDONLY | O_DIRECTORY);
    int tfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    memset(&d, 0, sizeof(d));
    if (dfd < 0 ||
        syscall(NR_quotactl_fd, dfd, QCMD(Q_GETQUOTA, USRQUOTA), QUSER, &d) != 0 ||
        d.dqb_bhardlimit != 256)
      STEP_FAIL("quotactl_fd errno %d", errno);
    errno = 0;
    char other[64];
    int other_ok = tfd >= 0 &&
                   syscall(NR_quotactl_fd, tfd, QCMD(Q_GETQUOTA, USRQUOTA),
                           QUSER, &d) == -1 &&
                   (errno == ENOSYS || errno == ENOTSUP);
    snprintf(other, sizeof(other), "%d", errno);
    if (!other_ok) {
      /* /tmp may itself be ext4 with quotas off: that answers ESRCH/EINVAL.
       * /proc never has quota operations. */
      int pfd = open("/proc", O_RDONLY | O_DIRECTORY);
      errno = 0;
      other_ok = pfd >= 0 &&
                 syscall(NR_quotactl_fd, pfd, QCMD(Q_GETQUOTA, USRQUOTA), QUSER,
                         &d) == -1 &&
                 errno == ENOSYS;
      snprintf(other, sizeof(other), "/tmp %s, /proc %d", other, errno);
    }
    if (!other_ok)
      STEP_FAIL("no-quota filesystem: %s (want ENOSYS)", other);
    close(dfd);
    if (tfd >= 0)
      close(tfd);
    ok(names[i++]);
  }

  /* quota-off */
  {
    struct fs_quota_stat_ qs;
    if (quotactl(QCMD(Q_QUOTAOFF, USRQUOTA), g_qdev, 0, 0) != 0)
      STEP_FAIL("Q_QUOTAOFF errno %d", errno);
    memset(&qs, 0, sizeof(qs));
    if (quotactl(QCMD(Q_XGETQSTAT_, USRQUOTA), g_qdev, 0, (caddr_t)&qs) != 0 ||
        !(qs.qs_flags & FS_QUOTA_UDQ_ACCT_) || (qs.qs_flags & FS_QUOTA_UDQ_ENFD_))
      STEP_FAIL("after Q_QUOTAOFF flags %x errno %d", qs.qs_flags, errno);
    int w = write_as_user(QMNT "/u/d", 512);
    if (w != 0)
      STEP_FAIL("write past the limit with enforcement off: %d", w);
    ok(names[i++]);
  }

  /* quota-tools */
  {
    struct dqblk d;
    if (sh("setquota -u 1000 0 4096 0 100 " QMNT " >/dev/null 2>&1") != 0)
      STEP_FAIL("setquota failed");
    if (getq(QUSER, &d) != 0 || d.dqb_bhardlimit != 4096 ||
        d.dqb_ihardlimit != 100)
      STEP_FAIL("after setquota: blocks %llu inodes %llu",
                (unsigned long long)d.dqb_bhardlimit,
                (unsigned long long)d.dqb_ihardlimit);
    if (sh("repquota -u " QMNT " 2>/dev/null | grep -q '^#1000 \\|^1000 \\|4096'") != 0)
      STEP_FAIL("repquota does not list the user");
    ok(names[i++]);
  }

  /* quota-persist */
  {
    struct dqblk d;
    if (umount(QMNT) != 0 || quota_mount() != 0)
      STEP_FAIL("remount errno %d", errno);
    if (getq(QUSER, &d) != 0 || d.dqb_bhardlimit != 4096 ||
        d.dqb_curspace < 64 * 1024)
      STEP_FAIL("after remount: limit %llu space %llu errno %d",
                (unsigned long long)d.dqb_bhardlimit,
                (unsigned long long)d.dqb_curspace, errno);
    if (umount(QMNT) != 0)
      STEP_FAIL("umount errno %d", errno);
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "e2fsck -fn %s >/dev/null 2>&1", g_qdev);
    int fsck = sh(cmd);
    if (fsck != 0)
      STEP_FAIL("e2fsck -fn exit %d", fsck);
    ok(names[i++]);
  }
#undef STEP_FAIL
  quota_teardown();
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--pkey-exec-probe") == 0)
    return pkey_exec_probe(argv[2]);
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
  if (!cpu_has_pkeys()) {
    run("pkey-absent", check_pkey_absent);
  } else {
    run("pkey-cpuinfo", check_pkey_cpuinfo);
    run("pkey-alloc", check_pkey_alloc);
    run("pkey-rights", check_pkey_rights);
    run("pkey-access-disable", check_pkey_access_disable);
    run("pkey-write-disable", check_pkey_write_disable);
    run("pkey-mprotect-keeps", check_pkey_mprotect_keeps);
    run("pkey-kernel-copy", check_pkey_kernel_copy);
    run("pkey-threads", check_pkey_threads);
    run("pkey-signal", check_pkey_signal);
    run("pkey-fork-exec", check_pkey_fork_exec);
    run("pkey-exec-only", check_pkey_exec_only);
  }
  quota_suite();
  marker(g_fail ? "M124-SMOKE: done with failures\n" : "M124-SMOKE: done\n");
  return 0;
}
