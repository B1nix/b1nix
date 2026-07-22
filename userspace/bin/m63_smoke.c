/* M63 smoke: seccomp-bpf syscall filtering.
 *
 * Each subtest runs in a forked child so the parent stays unfiltered and can
 * fork/waitpid freely. Filtered calls use the raw syscall() macro so they hit
 * the kernel (getpid() could be libc-cached). Markers are emitted by the parent
 * based on the child's real outcome — exit code or kill signal — never
 * unconditionally.
 *
 *   seccomp-errno   a filter returning ERRNO(EACCES) for SYS_GETPID makes that
 *                   call fail while an un-targeted call (SYS_GETPPID) still works.
 *   seccomp-kill    a filter returning KILL_PROCESS for SYS_GETPID terminates
 *                   the child with SIGSYS when it makes that call.
 *   seccomp-strict  SECCOMP_MODE_STRICT kills the child on any syscall outside
 *                   read/write/exit/sigreturn.
 *   seccomp-inherit a child forked AFTER the parent installed a filter inherits
 *                   it (the grandchild is killed by the inherited filter).
 *   seccomp-nnp     PR_SET_NO_NEW_PRIVS round-trips through PR_GET_NO_NEW_PRIVS.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <syscall.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#ifdef __linux__
#include <sys/syscall.h>
#define SYS_GETPID __NR_getpid
#define SYS_GETPPID __NR_getppid
#define seccomp(op, flags, args) syscall(__NR_seccomp, (op), (flags), (args))
#endif

static int g_fail;

static void marker(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}
static void ok(const char *name) {
  char buf[128];
  snprintf(buf, sizeof(buf), "M63-SMOKE: ok %s", name);
  marker(buf);
}
static void fail(const char *name) {
  char buf[128];
  snprintf(buf, sizeof(buf), "M63-SMOKE: FAIL %s", name);
  marker(buf);
  g_fail = 1;
}

/* Install a filter: action(verdict) for syscall `target`, ALLOW everything else.
 * offsetof(struct seccomp_data, nr) == 0. */
static int install_filter(int target, unsigned int verdict) {
  struct sock_filter prog[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),         /* A = seccomp_data.nr */
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, target, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, verdict),            /* matched: verdict */
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),  /* else: allow */
  };
  struct sock_fprog fprog = {.len = 4, .filter = prog};
  return seccomp(SECCOMP_SET_MODE_FILTER, 0, &fprog);
}

/* errno + allow, in a child. Child exits 0 iff getpid is denied with EACCES and
 * getppid still works. */
static void test_errno_allow(void) {
  pid_t c = fork();
  if (c == 0) {
    if (install_filter(SYS_GETPID, SECCOMP_RET_ERRNO | EACCES) != 0)
      _exit(2);
    /* musl's syscall() applies the error convention: a kernel -EACCES surfaces
     * as a -1 return with errno==EACCES, not a raw -13. */
    errno = 0;
    long denied = syscall(SYS_GETPID);
    int denied_errno = errno;
    long allowed = syscall(SYS_GETPPID); /* -> a real pid */
    _exit((denied == -1 && denied_errno == EACCES && allowed > 0) ? 0 : 1);
  }
  int st = 0;
  waitpid(c, &st, 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
    ok("seccomp-errno");
  else
    fail("seccomp-errno");
}

/* SECCOMP_RET_ERRNO with errno 0: the syscall must be BLOCKED (not run) yet
 * return 0 to userspace. getpid normally returns a real pid > 0, so a 0 result
 * proves the call was intercepted with a 0 errno rather than executed. */
static void test_errno_zero(void) {
  pid_t c = fork();
  if (c == 0) {
    if (install_filter(SYS_GETPID, SECCOMP_RET_ERRNO | 0) != 0)
      _exit(2);
    long r = syscall(SYS_GETPID); /* blocked, returns 0 (did NOT run) */
    _exit(r == 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(c, &st, 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
    ok("seccomp-errno-zero");
  else
    fail("seccomp-errno-zero");
}

static void test_kill(void) {
  pid_t c = fork();
  if (c == 0) {
    install_filter(SYS_GETPID, SECCOMP_RET_KILL_PROCESS);
    syscall(SYS_GETPID); /* killed here */
    _exit(99);           /* unreachable */
  }
  int st = 0;
  waitpid(c, &st, 0);
  if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS)
    ok("seccomp-kill");
  else
    fail("seccomp-kill");
}

static void test_strict(void) {
  pid_t c = fork();
  if (c == 0) {
    if (seccomp(SECCOMP_SET_MODE_STRICT, 0, 0) != 0)
      _exit(2);
    syscall(SYS_GETPID); /* not in the strict allow-list -> killed */
    _exit(99);
  }
  int st = 0;
  waitpid(c, &st, 0);
  if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS)
    ok("seccomp-strict");
  else
    fail("seccomp-strict");
}

/* Inheritance: a child installs a KILL filter, then forks a grandchild that
 * makes the forbidden call and must be killed by the inherited filter. */
static void test_inherit(void) {
  pid_t c = fork();
  if (c == 0) {
    install_filter(SYS_GETPID, SECCOMP_RET_KILL_PROCESS);
    pid_t g = fork();
    if (g == 0) {
      syscall(SYS_GETPID); /* killed by the inherited filter */
      _exit(99);
    }
    int gst = 0;
    waitpid(g, &gst, 0);
    _exit((WIFSIGNALED(gst) && WTERMSIG(gst) == SIGSYS) ? 0 : 1);
  }
  int st = 0;
  waitpid(c, &st, 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
    ok("seccomp-inherit");
  else
    fail("seccomp-inherit");
}

static void test_nnp(void) {
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    fail("seccomp-nnp");
    return;
  }
  if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
    fail("seccomp-nnp");
    return;
  }
  ok("seccomp-nnp");
}

int main(void) {
  marker("M63-SMOKE: start");
  test_errno_allow();
  test_errno_zero();
  test_kill();
  test_strict();
  test_inherit();
  test_nnp();
  marker(g_fail ? "M63-SMOKE: done (with failures)" : "M63-SMOKE: done");
  return g_fail ? 1 : 0;
}
