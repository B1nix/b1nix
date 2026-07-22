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
  if (ftruncate(fd, 100) < 0 || ftruncate(fd, 8192) < 0) {
    fail("truncate-zeros-ftruncate", errno, 0);
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
    snprintf(buf, sizeof(buf), "%lu\n", count);
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

  for (int i = 0; i < NICE_WORKERS * 2; i++) {
    snprintf(path, sizeof(path), "/tmp/m46nice_%c%d",
             i < NICE_WORKERS ? 'h' : 'l', i % NICE_WORKERS);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
      char buf[64];
      memset(buf, 0, sizeof(buf));
      read(fd, buf, sizeof(buf) - 1);
      if (i < NICE_WORKERS) {
        count_high += strtoul(buf, NULL, 10);
      } else {
        count_low += strtoul(buf, NULL, 10);
      }
      close(fd);
    }
    unlink(path);
  }

  if (count_high * 2 > count_low * 3) {
    ok("nice-biasing");
  } else {
    char dbg[128];
    snprintf(dbg, sizeof(dbg), "nice-biasing: high=%lu low=%lu", count_high, count_low);
    marker(dbg);
    fail("nice-biasing", count_high, count_low);
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
  /* The failure string must not contain "M46-SMOKE: done" — the host-side
   * grep is a substring match. */
  marker(g_fail ? "M46-SMOKE: completed-with-failures" : "M46-SMOKE: done");
  return g_fail;
}
