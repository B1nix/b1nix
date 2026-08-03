/*
 * M92: musl libc smoke test — real musl compilation and execution.
 *
 * This binary is compiled against musl libc (via b1nix-musl-cc) and tests
 * that musl's syscall wrappers work correctly through the Linux ABI layer.
 * Tests: basic I/O, malloc, printf, fork, signals, pthread.
 *
 * Compiled with: tools/b1nix-musl-cc userspace/bin/m92_musl_libc_test.c -o build/m92-musl-libc-test
 */

#define _GNU_SOURCE
#define _BSD_SOURCE
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

static int ok_count = 0;
static int fail_count = 0;

static void ok(const char *name) {
    ok_count++;
    printf("M92-MUSL: ok %s\n", name);
    fflush(stdout);
}

static void fail(const char *name, const char *reason) {
    fail_count++;
    printf("M92-MUSL: fail %s (%s)\n", name, reason);
    fflush(stdout);
}

/* Test 1: Basic write (hello world) */
static void test_write(void) {
    const char *msg = "M92-MUSL: hello from musl\n";
    ssize_t n = write(STDOUT_FILENO, msg, strlen(msg));
    if (n > 0)
        ok("write");
    else
        fail("write", "write returned <= 0");
}

/* Test 2: malloc/free */
static void test_malloc(void) {
    void *p = malloc(1024);
    if (p) {
        memset(p, 0xAB, 1024);
        free(p);
        ok("malloc");
    } else {
        fail("malloc", "malloc returned NULL");
    }
}

/* Test 3: printf formatting */
static void test_printf(void) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "test %d %s 0x%x", 42, "hello", 0xDEAD);
    if (n > 0 && strstr(buf, "42") && strstr(buf, "hello"))
        ok("printf");
    else
        fail("printf", "snprintf output mismatch");
}

/* Test 4: fork + waitpid */
static void test_fork(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* child */
        _exit(42);
    } else if (pid > 0) {
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);
        if (w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42)
            ok("fork+waitpid");
        else
            fail("fork+waitpid", "unexpected exit status");
    } else {
        fail("fork+waitpid", "fork failed");
    }
}

/* Test 5: signal handling */
static volatile int sig_received = 0;
static void sig_handler(int sig) {
    (void)sig;
    sig_received = 1;
}

static void test_signal(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGUSR1, &sa, NULL) == 0) {
        kill(getpid(), SIGUSR1);
        if (sig_received)
            ok("signal");
        else
            fail("signal", "handler not called");
    } else {
        fail("signal", "sigaction failed");
    }
}

/* Test 6: pipe */
static void test_pipe(void) {
    int fds[2];
    if (pipe(fds) == 0) {
        const char *msg = "pipe-test";
        write(fds[1], msg, strlen(msg));
        close(fds[1]);
        char buf[32] = {0};
        read(fds[0], buf, sizeof(buf) - 1);
        close(fds[0]);
        if (strcmp(buf, msg) == 0)
            ok("pipe");
        else
            fail("pipe", "data mismatch");
    } else {
        fail("pipe", "pipe() failed");
    }
}

/* Test 7: open/read/close (/dev/null) */
static void test_open(void) {
    int fd = open("/dev/null", 0);
    if (fd >= 0) {
        char buf[16];
        ssize_t n = read(fd, buf, sizeof(buf));
        /* /dev/null read returns 0, not error */
        close(fd);
        ok("open+read");
    } else {
        fail("open+read", "open /dev/null failed");
    }
}

/* Test 8: clock_gettime */
static void test_clock(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0 && ts.tv_sec > 0)
        ok("clock_gettime");
    else
        fail("clock_gettime", "clock_gettime failed");
}

/* Test 9: getenv/setenv */
static void test_env(void) {
    setenv("M92_TEST_VAR", "musl_works", 1);
    char *val = getenv("M92_TEST_VAR");
    if (val && strcmp(val, "musl_works") == 0)
        ok("getenv+setenv");
    else
        fail("getenv+setenv", "value mismatch");
}

/* Test 10: pthread_create + pthread_join */
#include <pthread.h>

static void *thread_func(void *arg) {
    int *result = (int *)arg;
    *result = 123;
    return NULL;
}

static void test_pthread(void) {
    pthread_t tid;
    int result = 0;
    if (pthread_create(&tid, NULL, thread_func, &result) == 0) {
        if (pthread_join(tid, NULL) == 0 && result == 123)
            ok("pthread");
        else
            fail("pthread", "join failed or result wrong");
    } else {
        fail("pthread", "pthread_create failed");
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("M92-MUSL: start\n");
    fflush(stdout);

    test_write();
    test_malloc();
    test_printf();
    test_fork();
    test_signal();
    test_pipe();
    test_open();
    test_clock();
    test_env();
    test_pthread();

    printf("M92-MUSL: done (ok=%d fail=%d)\n", ok_count, fail_count);
    fflush(stdout);

    return fail_count > 0 ? 1 : 0;
}
