/* M46 smoke: VFS integrity + POSIX process-conformance fixes.
 * Covers: exit-status encoding vs signal death, kill(0)/kill(-1), waitpid on
 * a process group, setpgid POSIX errnos, getpgid, nice/getpriority,
 * fork blocked-mask inheritance, O_APPEND offset-under-lock, truncate
 * shrink-then-grow zeroing, and direct execve() of a #! script. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/times.h>
#include <pthread.h>
#include <stdlib.h>
#include <sched.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/time.h>

static int g_fail;

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

static void ok(const char *name) {
  char buf[128];
  snprintf(buf, sizeof(buf), "M46-SMOKE: ok %s", name);
  marker(buf);
}

static void fail(const char *name, long a, long b) {
  char buf[160];
  snprintf(buf, sizeof(buf), "M46-SMOKE: FAIL %s got=%ld expected=%ld", name,
           a, b);
  marker(buf);
  g_fail = 1;
}

static volatile sig_atomic_t got_usr1;
static void usr1_handler(int sig) {
  (void)sig;
  got_usr1 = 1;
}

/* A child exiting with code 139 (= the old 128+SIGSEGV ambiguity, and what
 * the libc assert() does) must be reported as a NORMAL exit. */
static void test_exit_status(void) {
  pid_t pid = fork();
  if (pid == 0)
    _exit(139);
  int st = 0;
  if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
      WEXITSTATUS(st) != 139)
    fail("exit-status-139", st, 139 << 8);
  else
    ok("exit-status-139");

  pid = fork();
  if (pid == 0) {
    kill(getpid(), SIGKILL);
    for (;;)
      (void)sched_yield();
  }
  st = 0;
  if (waitpid(pid, &st, 0) != pid || !WIFSIGNALED(st) ||
      WTERMSIG(st) != SIGKILL)
    fail("signal-death", st, SIGKILL);
  else
    ok("signal-death");
}

/* kill(0, sig) signals the CALLER's process group. Move into our own group
 * first so the test driver is not signalled. */
static void test_kill_zero(void) {
  if (setpgid(0, 0) < 0) {
    fail("kill-zero-setpgid", errno, 0);
    return;
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = usr1_handler;
  sigaction(SIGUSR1, &sa, 0);
  got_usr1 = 0;
  if (kill(0, SIGUSR1) < 0) {
    fail("kill-zero-pgrp", errno, 0);
    return;
  }
  for (int i = 0; i < 100 && !got_usr1; i++)
    (void)sched_yield();
  if (got_usr1)
    ok("kill-zero-pgrp");
  else
    fail("kill-zero-pgrp", 0, 1);
}

/* kill(-1, 0) is the broadcast permission probe: must succeed (other
 * processes exist) without delivering anything. */
static void test_kill_all_probe(void) {
  if (kill(-1, 0) == 0)
    ok("kill-all-probe");
  else
    fail("kill-all-probe", errno, 0);
}

/* waitpid(-pgid) reaps a child from that process group. */
static void test_waitpid_pgid(void) {
  pid_t pid = fork();
  if (pid == 0) {
    setpgid(0, 0);
    _exit(7);
  }
  /* Both sides race to set the group (the POSIX idiom); either wins. */
  (void)setpgid(pid, pid);
  int st = 0;
  pid_t got = waitpid(-pid, &st, 0);
  if (got == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7)
    ok("waitpid-pgid");
  else
    fail("waitpid-pgid", got, pid);
}

static void test_setpgid_errnos(void) {
  errno = 0;
  if (setpgid(99999, 0) < 0 && errno == ESRCH)
    ok("setpgid-esrch");
  else
    fail("setpgid-esrch", errno, ESRCH);

  errno = 0;
  if (setpgid(0, 99999) < 0 && errno == EPERM)
    ok("setpgid-eperm-pgrp");
  else
    fail("setpgid-eperm-pgrp", errno, EPERM);
}

static void test_getpgid(void) {
  pid_t a = getpgid(0);
  pid_t b = getpgrp();
  if (a > 0 && a == b)
    ok("getpgid");
  else
    fail("getpgid", a, b);
  errno = 0;
  if (getpgid(99999) < 0 && errno == ESRCH)
    ok("getpgid-esrch");
  else
    fail("getpgid-esrch", errno, ESRCH);
}

static void test_nice(void) {
  int n = nice(5);
  if (n < 0 && errno != 0) {
    fail("nice-roundtrip", errno, 0);
    return;
  }
  int got = getpriority(PRIO_PROCESS, 0);
  if (got == n && n >= 5)
    ok("nice-roundtrip");
  else
    fail("nice-roundtrip", got, n);
  (void)setpriority(PRIO_PROCESS, 0, 0);
}

/* POSIX fork: the child inherits the parent's blocked-signal mask. */
static void test_fork_sigmask(void) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR2);
  sigprocmask(SIG_BLOCK, &set, 0);
  pid_t pid = fork();
  if (pid == 0) {
    sigset_t cur;
    sigemptyset(&cur);
    sigprocmask(SIG_BLOCK, 0, &cur);
    _exit(sigismember(&cur, SIGUSR2) == 1 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  sigprocmask(SIG_UNBLOCK, &set, 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
    ok("fork-sigmask");
  else
    fail("fork-sigmask", st, 0);
}

/* Two processes appending records concurrently: with the O_APPEND offset
 * sampled under the inode lock, no record may overwrite another — the file
 * must contain exactly 2*N records. */
#define APPEND_RECS 64
#define APPEND_RECSZ 32
static void append_worker(const char *path, char tag) {
  int fd = open(path, O_WRONLY | O_APPEND);
  if (fd < 0)
    _exit(1);
  char rec[APPEND_RECSZ];
  memset(rec, tag, sizeof(rec));
  rec[APPEND_RECSZ - 1] = '\n';
  for (int i = 0; i < APPEND_RECS; i++) {
    if (write(fd, rec, sizeof(rec)) != (ssize_t)sizeof(rec))
      _exit(1);
    if ((i & 7) == 0)
      (void)sched_yield();
  }
  close(fd);
  _exit(0);
}

static void test_append_atomic(void) {
  const char *path = "/tmp/m46append";
  unlink(path);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("append-atomic-open", errno, 0);
    return;
  }
  close(fd);
  pid_t a = fork();
  if (a == 0)
    append_worker(path, 'A');
  pid_t b = fork();
  if (b == 0)
    append_worker(path, 'B');
  int st = 0;
  waitpid(a, &st, 0);
  waitpid(b, &st, 0);
  struct stat sb;
  if (stat(path, &sb) < 0 ||
      sb.st_size != (off_t)(2 * APPEND_RECS * APPEND_RECSZ)) {
    fail("append-atomic", (long)sb.st_size, 2 * APPEND_RECS * APPEND_RECSZ);
    return;
  }
  ok("append-atomic");
  unlink(path);
}

/* Shrink-then-grow: bytes between the shrink point and the regrown size must
 * read back as zeros, never the pre-truncate contents. */
static void test_truncate_zeros(void) {
  const char *path = "/tmp/m46trunc";
  unlink(path);
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fail("truncate-zeros-open", errno, 0);
    return;
  }
  char buf[512];
  memset(buf, 'A', sizeof(buf));
  for (int i = 0; i < 16; i++) {
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
      fail("truncate-zeros-write", errno, 0);
      close(fd);
      return;
    }
  }
  if (ftruncate(fd, 100) < 0) {
    fail("truncate-zeros-ftruncate100", errno, 0);
    close(fd);
    return;
  }
  if (ftruncate(fd, 8192) < 0) {
    fail("truncate-zeros-ftruncate8192", errno, 0);
    close(fd);
    return;
  }
  if (lseek(fd, 4096, SEEK_SET) != 4096 ||
      read(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
    fail("truncate-zeros-read", errno, 0);
    close(fd);
    return;
  }
  for (unsigned i = 0; i < sizeof(buf); i++) {
    if (buf[i] != 0) {
      fail("truncate-zeros", buf[i], 0);
      close(fd);
      return;
    }
  }
  close(fd);
  unlink(path);
  ok("truncate-zeros");
}

/* Direct execve() of a #! file must run the interpreter (no shell retry). */
static void test_shebang(void) {
  const char *path = "/tmp/m46sh.sh";
  unlink(path);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd < 0) {
    fail("shebang-create", errno, 0);
    return;
  }
  const char *script = "#!/bin/sh\necho M46-SHEBANG-OK\n";
  write(fd, script, strlen(script));
  close(fd);
  chmod(path, 0755);
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = {(char *)path, 0};
    char *envp[] = {0};
    execve(path, argv, envp);
    _exit(127); /* execve failed */
  }
  int st = 0;
  waitpid(pid, &st, 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
    ok("shebang-exec");
  else
    fail("shebang-exec", st, 0);
  unlink(path);
}

static void *exit_group_thread(void *arg) {
  (void)arg;
  for (;;) {
    (void)sched_yield();
  }
  return 0;
}

static void test_exit_group(void) {
  pid_t pid = fork();
  if (pid == 0) {
    pthread_t th;
    pthread_create(&th, 0, exit_group_thread, 0);
    for (int i = 0; i < 10; i++) {
      (void)sched_yield();
    }
    _exit(42);
  }
  int st = 0;
  pid_t got = waitpid(pid, &st, 0);
  if (got == pid && WIFEXITED(st) && WEXITSTATUS(st) == 42) {
    ok("exit-group");
  } else {
    fail("exit-group", got, pid);
  }
}

static void test_resuid_resgid(void) {
  pid_t pid = fork();
  if (pid == 0) {
    if (setresgid(4000, 5000, 6000) < 0) {
      _exit(1);
    }
    if (getgid() != 4000 || getegid() != 5000) {
      _exit(2);
    }
    if (setresuid(1000, 2000, 3000) < 0) {
      _exit(3);
    }
    if (getuid() != 1000 || geteuid() != 2000) {
      _exit(4);
    }
    if (setresuid(0, 0, 0) == 0 || errno != EPERM) {
      _exit(5);
    }
    if (setresgid(0, 0, 0) == 0 || errno != EPERM) {
      _exit(6);
    }
    _exit(0);
  }

  int st = 0;
  pid_t got = waitpid(pid, &st, 0);
  if (got == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
    ok("setresuid-setresgid");
  } else {
    fail("setresuid-setresgid", WEXITSTATUS(st), 0);
  }
}

static void test_waitid(void) {
  pid_t pid = fork();
  if (pid == 0) {
    _exit(12);
  }
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  int rc = waitid(P_PID, pid, &info, WEXITED);
  if (rc == 0 && info.si_pid == pid && info.si_status == 12 && info.si_code == 1 /* CLD_EXITED */) {
    ok("waitid");
  } else {
    fail("waitid", rc, 0);
  }
}

static void test_times_rusage(void) {
  struct tms t;
  clock_t clk = times(&t);
  if (clk == (clock_t)-1) {
    fail("times-call", clk, 0);
    return;
  }
  struct rusage ru;
  int rc = getrusage(RUSAGE_SELF, &ru);
  if (rc < 0) {
    fail("getrusage-call", rc, 0);
    return;
  }
  ok("times-getrusage");
}

static volatile sig_atomic_t got_sighup;
static void sighup_handler(int sig) {
  (void)sig;
  got_sighup = 1;
}

static void test_orphaned_pgrp(void) {
  const char *path = "/tmp/m46orphan";
  unlink(path);
  got_sighup = 0;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sighup_handler;
  sigaction(SIGHUP, &sa, 0);

  pid_t p_pid = fork();
  if (p_pid == 0) {
    setpgid(0, 0);
    pid_t grandchild = fork();
    if (grandchild == 0) {
      kill(getpid(), SIGSTOP);
      for (int i = 0; i < 1000 && !got_sighup; i++) {
        (void)sched_yield();
      }
      if (got_sighup) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
          write(fd, "OK\n", 3);
          close(fd);
        }
      }
      _exit(0);
    }
    int st = 0;
    waitpid(grandchild, &st, WUNTRACED);
    _exit(0);
  }
  int st = 0;
  waitpid(p_pid, &st, 0);
  for (int i = 0; i < 50; i++) {
    (void)sched_yield();
  }
  int fd = open(path, O_RDONLY);
  char buf[32];
  memset(buf, 0, sizeof(buf));
  if (fd >= 0) {
    read(fd, buf, sizeof(buf));
    close(fd);
  }
  unlink(path);
  if (strcmp(buf, "OK\n") == 0) {
    ok("orphaned-pgrp");
  } else {
    fail("orphaned-pgrp", 0, 1);
  }
}

static void nice_worker(const char *path, int nice_val, int start_fd) {
  char token;
  if (read(start_fd, &token, 1) != 1) {
    _exit(1);
  }
  close(start_fd);
  nice(nice_val);
  /* Report the nice value the kernel actually stored, so a failure can tell
   * "nice() never took effect" apart from "the scheduler ignores nice". */
  int applied = getpriority(PRIO_PROCESS, 0);
  volatile unsigned long count = 0;
  struct timeval start, now;
  gettimeofday(&start, NULL);
  while (1) {
    count++;
    /* Cooperatively yield each iteration: this is the "cooperative stride
     * scheduling" test, and b1nix only preempts on the BSP (APs run the
     * cooperative model — a pure busy-spin on an AP is never time-sliced, so it
     * would monopolise that core and swamp the nice bias). Yielding at the
     * scheduler each pass lets the stride scheduler apply the nice weighting on
     * every CPU, exactly as a well-behaved cooperative workload would. */
    sched_yield();
    gettimeofday(&now, NULL);
    long ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
    if (ms >= 150) {
      break;
    }
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lu %d\n", count, applied);
    write(fd, buf, strlen(buf));
    close(fd);
  }
  _exit(0);
}

#define NICE_WORKERS 4

static void test_nice_biasing(void) {
  int barrier[2];
  if (pipe(barrier) < 0) {
    fail("nice-biasing", errno, 0);
    return;
  }

  unsigned long count_high = 0;
  unsigned long count_low = 0;
  pid_t pids[NICE_WORKERS * 2];
  char path[64];

  for (int i = 0; i < NICE_WORKERS * 2; i++) {
    snprintf(path, sizeof(path), "/tmp/m46nice_%c%d",
             i < NICE_WORKERS ? 'h' : 'l', i % NICE_WORKERS);
    unlink(path);
    pids[i] = fork();
    if (pids[i] == 0) {
      close(barrier[1]);
      nice_worker(path, i < NICE_WORKERS ? -20 : 19, barrier[0]);
    }
  }

  close(barrier[0]);
  char starts[NICE_WORKERS * 2];
  memset(starts, 1, sizeof(starts));
  write(barrier[1], starts, sizeof(starts));
  close(barrier[1]);

  int st = 0;
  for (int i = 0; i < NICE_WORKERS * 2; i++) {
    waitpid(pids[i], &st, 0);
  }

  int nice_high = 999, nice_low = 999;
  for (int i = 0; i < NICE_WORKERS * 2; i++) {
    snprintf(path, sizeof(path), "/tmp/m46nice_%c%d",
             i < NICE_WORKERS ? 'h' : 'l', i % NICE_WORKERS);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
      char buf[64];
      memset(buf, 0, sizeof(buf));
      read(fd, buf, sizeof(buf) - 1);
      char *end = NULL;
      unsigned long c = strtoul(buf, &end, 10);
      int applied = end ? (int)strtol(end, NULL, 10) : 999;
      if (i < NICE_WORKERS) {
        count_high += c;
        nice_high = applied;
      } else {
        count_low += c;
        nice_low = applied;
      }
      close(fd);
    }
    unlink(path);
  }

  if (count_high * 2 > count_low * 3) {
    ok("nice-biasing");
  } else {
    char dbg[128];
    snprintf(dbg, sizeof(dbg),
             "nice-biasing: high=%lu low=%lu applied_nice high=%d low=%d",
             count_high, count_low, nice_high, nice_low);
    marker(dbg);
    fail("nice-biasing", count_high, count_low);
  }
}


/* ── Directory timestamps ──────────────────────────────────────────────────
 * POSIX: creating, removing or renaming an entry marks the CONTAINING
 * directory modified. Nothing here did, so a directory's mtime was fixed at
 * the moment it was created. systemd re-reads a unit directory only when its
 * mtime differs from the one its last scan recorded, so a unit file written
 * afterwards was invisible for ever -- and the second half of that is
 * resolution: whole seconds are too coarse to separate two writes, so the
 * check below makes both changes inside one second on purpose. */
static int dir_stamp(const char *path, long long *sec, long *nsec) {
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  *sec = (long long)st.st_mtime;
  *nsec = (long)st.st_mtim.tv_nsec;
  return 0;
}

static void test_dir_mtime(void) {
  const char *dir = "/tmp/m46dirmtime";
  char f1[64], f2[64];
  long long s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  long n0 = 0, n1 = 0, n2 = 0, n3 = 0;

  snprintf(f1, sizeof(f1), "%s/a", dir);
  snprintf(f2, sizeof(f2), "%s/b", dir);
  unlink(f1);
  unlink(f2);
  rmdir(dir);
  if (mkdir(dir, 0755) != 0) {
    fail("dir-mtime-create", errno, 0);
    return;
  }
  if (dir_stamp(dir, &s0, &n0) != 0) {
    fail("dir-mtime-create", errno, 1);
    return;
  }

  int fd = open(f1, O_CREAT | O_WRONLY, 0644);
  if (fd < 0) {
    fail("dir-mtime-create", errno, 2);
    return;
  }
  close(fd);
  if (dir_stamp(dir, &s1, &n1) != 0 || (s1 == s0 && n1 == n0)) {
    fail("dir-mtime-create", (int)(s1 - s0), (int)(n1 - n0));
  } else {
    ok("dir-mtime-create");
  }

  /* A second change inside the same second must still be visible: this is the
   * sub-second half, and it is the half systemd depends on. */
  fd = open(f2, O_CREAT | O_WRONLY, 0644);
  if (fd >= 0)
    close(fd);
  if (dir_stamp(dir, &s2, &n2) != 0 || (s2 == s1 && n2 == n1)) {
    fail("dir-mtime-subsecond", (int)(s2 - s1), (int)(n2 - n1));
  } else {
    ok("dir-mtime-subsecond");
  }

  unlink(f2);
  if (dir_stamp(dir, &s3, &n3) != 0 || (s3 == s2 && n3 == n2)) {
    fail("dir-mtime-unlink", (int)(s3 - s2), (int)(n3 - n2));
  } else {
    ok("dir-mtime-unlink");
  }

  unlink(f1);
  rmdir(dir);
}

/* ── rename(2) keeps the WHOLE name ────────────────────────────────────────
 * The rename path copied the new name into a 64-byte field while every other
 * creation path stores VFS_NAME_MAX-1, so a destination longer than 63
 * characters was silently renamed to a shorter, different name: the file
 * existed under a name nobody would look up, and the intended one was ENOENT.
 * "Write to a temporary, rename over the target" is how systemd, dpkg and
 * glibc all replace a file, and their temporary names are long. */
static void test_rename_long_name(void) {
  char src[80], dst[200];
  char longname[130];
  struct stat st;
  int fd;

  for (size_t i = 0; i < sizeof(longname) - 1; i++)
    longname[i] = 'n';
  longname[sizeof(longname) - 1] = '\0';
  snprintf(src, sizeof(src), "/tmp/m46ren-src");
  snprintf(dst, sizeof(dst), "/tmp/%s", longname);
  unlink(src);
  unlink(dst);

  fd = open(src, O_CREAT | O_WRONLY, 0644);
  if (fd < 0) {
    fail("rename-long-name", errno, 0);
    return;
  }
  write(fd, "x", 1);
  close(fd);

  if (rename(src, dst) != 0) {
    fail("rename-long-name", errno, 1);
    unlink(src);
    return;
  }
  if (stat(dst, &st) != 0) {
    fail("rename-long-name", errno, 2);
  } else if (stat(src, &st) == 0) {
    fail("rename-long-name", 0, 3); /* the old name must be gone */
  } else {
    ok("rename-long-name");
  }
  unlink(dst);
  unlink(src);
}

/* ── A datagram with no bytes in it is still a datagram ────────────────────
 * Linux delivers a zero-length datagram and the ancillary data attached to it,
 * and a program can send one over a socketpair purely to be identified by the
 * SCM_CREDENTIALS its peer's SO_PASSCRED attaches. Three halves of that were
 * missing: the send was a no-op, the receive refused a zero-length iovec with
 * EINVAL, and a queued empty message never made the socket readable. The
 * existing credentials check uses a STREAM socket and a four-byte message,
 * which is why it passed throughout. */
static void test_empty_datagram(void) {
  int sv[2];
  int on = 1;
  pid_t pid;
  struct msghdr m;
  struct iovec iov;
  char cbuf[CMSG_SPACE(sizeof(struct ucred))];
  struct cmsghdr *c;
  struct ucred cred;
  struct pollfd p;
  int n, status = 0;
  char dummy = 0;

  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
    fail("empty-datagram", errno, 0);
    return;
  }
  if (setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0) {
    fail("empty-datagram", errno, 1);
    close(sv[0]);
    close(sv[1]);
    return;
  }

  pid = fork();
  if (pid < 0) {
    fail("empty-datagram", errno, 2);
    close(sv[0]);
    close(sv[1]);
    return;
  }
  if (pid == 0) {
    close(sv[0]);
    /* A write of zero bytes on a datagram socket: an empty message. */
    _exit(write(sv[1], &dummy, 0) == 0 ? 0 : 1);
  }
  close(sv[1]);

  p.fd = sv[0];
  p.events = POLLIN;
  if (poll(&p, 1, 5000) <= 0) {
    fail("empty-datagram", errno, 3); /* never became readable */
    waitpid(pid, &status, 0);
    close(sv[0]);
    return;
  }

  memset(&m, 0, sizeof(m));
  iov.iov_base = &dummy;
  iov.iov_len = 0; /* a zero-length iovec, as the manager uses */
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = cbuf;
  m.msg_controllen = sizeof(cbuf);
  n = (int)recvmsg(sv[0], &m, 0);
  waitpid(pid, &status, 0);

  if (n != 0) {
    fail("empty-datagram", n, errno);
    close(sv[0]);
    return;
  }
  c = CMSG_FIRSTHDR(&m);
  if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_CREDENTIALS) {
    fail("empty-datagram", 4, 0); /* arrived carrying no identity */
    close(sv[0]);
    return;
  }
  memcpy(&cred, CMSG_DATA(c), sizeof(cred));
  if ((pid_t)cred.pid != pid) {
    fail("empty-datagram", (int)cred.pid, (int)pid);
    close(sv[0]);
    return;
  }
  ok("empty-datagram-cred");
  close(sv[0]);
}

/* ── A read-only mount refuses the OPEN ────────────────────────────────────
 * `ProtectSystem=strict` binds a path onto itself and remounts it read-only.
 * Two things were wrong: the mount lookup returned the OLDEST entry rooted at
 * a node rather than the newest, so the read-only remount was recorded and
 * never consulted; and open(2) never checked the mount at all, so O_WRONLY
 * succeeded and O_TRUNC emptied a file on a filesystem the kernel believed
 * was read-only.
 *
 * Run inside its own mount namespace so a failure cannot leave the rest of
 * the suite looking at a read-only /tmp. */
static void test_readonly_mount(void) {
  pid_t pid = fork();
  int status = 0;

  if (pid < 0) {
    fail("rdonly-mount-open", errno, 0);
    return;
  }
  if (pid == 0) {
    const char *dir = "/tmp/m46ro";
    char file[64];
    int fd;

    snprintf(file, sizeof(file), "%s/f", dir);
    mkdir(dir, 0755);
    fd = open(file, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
      _exit(11);
    if (write(fd, "keepme", 6) != 6)
      _exit(12);
    close(fd);

    if (unshare(CLONE_NEWNS) != 0)
      _exit(13);
    if (mount(dir, dir, NULL, MS_BIND, NULL) != 0)
      _exit(14);
    if (mount(NULL, dir, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) != 0)
      _exit(15);

    /* Reading is still allowed. */
    fd = open(file, O_RDONLY);
    if (fd < 0)
      _exit(16);
    close(fd);

    if (open(file, O_WRONLY) >= 0)
      _exit(17);
    if (errno != EROFS)
      _exit(18);
    if (open(file, O_WRONLY | O_TRUNC) >= 0)
      _exit(19);
    if (errno != EROFS)
      _exit(20);
    if (open("/tmp/m46ro/new", O_CREAT | O_WRONLY, 0644) >= 0)
      _exit(21);
    if (errno != EROFS)
      _exit(22);

    /* And the file it was told not to touch still has its contents. */
    {
      struct stat st;
      if (stat(file, &st) != 0 || st.st_size != 6)
        _exit(23);
    }
    _exit(0);
  }

  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    ok("rdonly-mount-open");
  } else {
    fail("rdonly-mount-open", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
         status);
  }
  unlink("/tmp/m46ro/f");
  rmdir("/tmp/m46ro");
}


/* ── A signal ends a sleep ─────────────────────────────────────────────────
 * SIGTERM's default action is to terminate, and a process parked in
 * nanosleep(2) is not exempt: `kill` reaches it and it dies there. Anything
 * that bounds a command depends on this — `timeout(1)` sends its child a
 * signal and expects the child to be gone — and a bound that returns the right
 * answer without bounding anything is worse than no bound at all.
 *
 * The check measures elapsed time as well as the exit status, because a child
 * that slept its full thirty seconds and then exited normally would satisfy a
 * status check made afterwards. */
static void test_signal_ends_sleep(void) {
  struct timespec t0, t1;
  pid_t pid;
  int status = 0;
  long elapsed_ms;

  clock_gettime(CLOCK_MONOTONIC, &t0);
  pid = fork();
  if (pid < 0) {
    fail("signal-ends-sleep", errno, 0);
    return;
  }
  if (pid == 0) {
    struct timespec s = { 5, 0 };
    sigset_t none;
    /* An IGNORED disposition survives fork AND exec, and the shell that
     * launches this suite runs `trap '' TERM`, so the default has to be put
     * back explicitly — otherwise this measures the trap, not the kernel.
     * Same for the mask. */
    signal(SIGTERM, SIG_DFL);
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    nanosleep(&s, NULL);
    _exit(7); /* reached only if the sleep ran to completion */
  }

  /* Long enough that the child is certainly inside the sleep. */
  {
    struct timespec w = { 0, 200 * 1000 * 1000 };
    nanosleep(&w, NULL);
  }
  if (kill(pid, SIGTERM) != 0) {
    fail("signal-ends-sleep", errno, 1);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return;
  }
  waitpid(pid, &status, 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  elapsed_ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                      (t1.tv_nsec - t0.tv_nsec) / 1000000);

  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
    fail("signal-ends-sleep", status, (int)elapsed_ms);
  } else if (elapsed_ms > 2000) {
    /* Signalled, but only after the five-second sleep had all but finished. */
    fail("signal-ends-sleep", -1, (int)elapsed_ms);
  } else {
    ok("signal-ends-sleep");
  }
}

int main(void) {
  marker("M46-SMOKE: start");
  test_exit_status();
  test_kill_zero();
  test_kill_all_probe();
  test_waitpid_pgid();
  test_setpgid_errnos();
  test_getpgid();
  test_nice();
  test_fork_sigmask();
  test_append_atomic();
  test_truncate_zeros();
  test_shebang();
  test_exit_group();
  test_resuid_resgid();
  test_waitid();
  test_times_rusage();
  test_orphaned_pgrp();
  test_nice_biasing();
  test_dir_mtime();
  test_rename_long_name();
  test_empty_datagram();
  test_readonly_mount();
  test_signal_ends_sleep();
  /* The failure string must not contain "M46-SMOKE: done" — the host-side
   * grep is a substring match. */
  marker(g_fail ? "M46-SMOKE: completed-with-failures" : "M46-SMOKE: done");
  return g_fail;
}
