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
#include <syscall.h>
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
      (void)syscall(SYS_YIELD);
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
    (void)syscall(SYS_YIELD);
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
      (void)syscall(SYS_YIELD);
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
  /* The failure string must not contain "M46-SMOKE: done" — the host-side
   * grep is a substring match. */
  marker(g_fail ? "M46-SMOKE: completed-with-failures" : "M46-SMOKE: done");
  return g_fail;
}
