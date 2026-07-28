/*
 * Simple POSIX smoke test for musl libc — verifies the build pipeline works.
 * Uses only standard POSIX APIs, no b1nix-specific syscalls.
 */
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

static void marker(const char *text) {
    write(1, text, strlen(text));
}

static void test_write(void) {
    const char *msg = "MUSL-POSIX: write ok\n";
    ssize_t n = write(STDOUT_FILENO, msg, strlen(msg));
    marker(n > 0 ? "MUSL-POSIX: ok write\n" : "MUSL-POSIX: fail write\n");
}

static void test_malloc(void) {
    void *p = malloc(256);
    if (p) {
        memset(p, 0xAB, 256);
        free(p);
        marker("MUSL-POSIX: ok malloc\n");
    } else {
        marker("MUSL-POSIX: fail malloc\n");
    }
}

static void test_fork(void) {
    pid_t pid = fork();
    if (pid == 0) {
        _exit(42);
    } else if (pid > 0) {
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);
        marker(w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42
               ? "MUSL-POSIX: ok fork\n" : "MUSL-POSIX: fail fork\n");
    } else {
        marker("MUSL-POSIX: fail fork\n");
    }
}

static void test_getenv(void) {
    setenv("MUSL_TEST_VAR", "hello", 1);
    const char *v = getenv("MUSL_TEST_VAR");
    marker(v && strcmp(v, "hello") == 0 ? "MUSL-POSIX: ok getenv\n" : "MUSL-POSIX: fail getenv\n");
}

/* ── Linux ABI conformance (M94) ──────────────────────────────────────────
 * Each of these reached the kernel as "unmapped syscall -> -ENOSYS" until the
 * translation table and the two new handlers landed. They are exercised through
 * musl, i.e. the exact path every ported program takes. */

static void test_clock_getres(void) {
    struct timespec res;
    int rc = clock_getres(CLOCK_MONOTONIC, &res);
    /* b1nix drives its clocks off the 100 Hz tick, so the resolution is 10 ms —
     * a caller sizing a poll loop from it must get the real number. */
    marker(rc == 0 && res.tv_sec == 0 && res.tv_nsec == 10000000
           ? "MUSL-POSIX: ok clock-getres\n" : "MUSL-POSIX: fail clock-getres\n");
}

static void test_times(void) {
    struct tms tb;
    clock_t t = times(&tb);
    marker(t != (clock_t)-1 ? "MUSL-POSIX: ok times\n"
                            : "MUSL-POSIX: fail times\n");
}

static void test_sysinfo(void) {
    struct sysinfo si;
    int rc = sysinfo(&si);
    marker(rc == 0 && si.totalram > 0 ? "MUSL-POSIX: ok sysinfo\n"
                                      : "MUSL-POSIX: fail sysinfo\n");
}

static void test_sched_getaffinity(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    int rc = sched_getaffinity(0, sizeof(set), &set);
    marker(rc == 0 && CPU_COUNT(&set) >= 1
           ? "MUSL-POSIX: ok sched-getaffinity\n"
           : "MUSL-POSIX: fail sched-getaffinity\n");
}

/* statx and faccessat2 through a REAL dirfd — not AT_FDCWD. That is the part
 * the kernel used to reject with EBADF because it had no per-fd resolution. */
static void test_statx_dirfd(void) {
    int dirfd = open("/etc", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) { marker("MUSL-POSIX: fail statx-dirfd\n"); return; }
    struct statx stx;
    memset(&stx, 0, sizeof(stx));
    long rc = syscall(SYS_statx, dirfd, "passwd", 0, 0x7ffu, &stx);
    int faccess = (int)syscall(SYS_faccessat2, dirfd, "passwd", R_OK, 0);
    close(dirfd);
    marker(rc == 0 && stx.stx_size > 0 ? "MUSL-POSIX: ok statx-dirfd\n"
                                       : "MUSL-POSIX: fail statx-dirfd\n");
    marker(faccess == 0 ? "MUSL-POSIX: ok faccessat2\n"
                        : "MUSL-POSIX: fail faccessat2\n");
}

static void test_sigtimedwait(void) {
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, &old) != 0) {
        marker("MUSL-POSIX: fail sigtimedwait\n");
        return;
    }
    raise(SIGUSR1);
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    struct timespec ts = { 2, 0 };
    int got = sigtimedwait(&set, &info, &ts);

    /* And the timeout path: nothing pending now, so it must return EAGAIN
     * rather than blocking forever or reporting a signal that never came. */
    struct timespec quick = { 0, 20000000 };
    int timed_out = sigtimedwait(&set, &info, &quick);
    int timed_out_errno = errno;
    sigprocmask(SIG_SETMASK, &old, NULL);

    marker(got == SIGUSR1 ? "MUSL-POSIX: ok sigtimedwait\n"
                          : "MUSL-POSIX: fail sigtimedwait\n");
    marker(timed_out == -1 && timed_out_errno == EAGAIN
           ? "MUSL-POSIX: ok sigtimedwait-timeout\n"
           : "MUSL-POSIX: fail sigtimedwait-timeout\n");
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    marker("MUSL-POSIX: start\n");
    test_write();
    test_malloc();
    test_fork();
    test_getenv();
    test_clock_getres();
    test_times();
    test_sysinfo();
    test_sched_getaffinity();
    test_statx_dirfd();
    test_sigtimedwait();
    marker("MUSL-POSIX: done\n");
    return 0;
}
