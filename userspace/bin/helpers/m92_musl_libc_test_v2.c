/*
 * M92: musl smoke test v2 — uses write() for all output, no printf.
 * Tests: write, malloc, fork, signal, pipe, clock, env, pthread.
 */
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>

static int ok_count = 0;
static int fail_count = 0;

static void raw_out(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
}

static void raw_out_n(const char *s, int len) {
    write(STDOUT_FILENO, s, len);
}

static void test_result(const char *name, int pass) {
    if (pass) {
        ok_count++;
        raw_out("M92-MUSL: ok ");
    } else {
        fail_count++;
        raw_out("M92-MUSL: fail ");
    }
    raw_out(name);
    raw_out("\n");
}

/* Test 1: write */
static void test_write(void) {
    const char *msg = "M92-MUSL: hello from musl\n";
    ssize_t n = write(STDOUT_FILENO, msg, strlen(msg));
    test_result("write", n > 0);
}

/* Test 2: malloc/free */
static void test_malloc(void) {
    raw_out("M92-MUSL: before malloc\n");
    void *p = malloc(1024);
    raw_out("M92-MUSL: after malloc\n");
    if (p) {
        memset(p, 0xAB, 1024);
        raw_out("M92-MUSL: before free\n");
        free(p);
        raw_out("M92-MUSL: after free\n");
        test_result("malloc", 1);
    } else {
        test_result("malloc", 0);
    }
}

/* Test 3: fork + waitpid */
static void test_fork(void) {
    pid_t pid = fork();
    if (pid == 0) {
        _exit(42);
    } else if (pid > 0) {
        int status = 0;
        pid_t w = waitpid(pid, &status, 0);
        test_result("fork+waitpid", w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42);
    } else {
        test_result("fork+waitpid", 0);
    }
}

/* Test 9: signal handling — uses SIGUSR2 to avoid conflict with musl's
 * internal SIGUSR1 usage in pthread. */
static volatile int sig_received = 0;
static void sig_handler(int sig) { (void)sig; sig_received = 1; }

static void test_signal(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR2, &sa, NULL) == 0) {
        kill(getpid(), SIGUSR2);
        test_result("signal", sig_received);
    } else {
        test_result("signal", 0);
    }
}

/* Test 5: pipe */
static void test_pipe(void) {
    int fds[2];
    if (pipe(fds) == 0) {
        const char *msg = "pipe-test";
        write(fds[1], msg, strlen(msg));
        close(fds[1]);
        char buf[32] = {0};
        read(fds[0], buf, sizeof(buf) - 1);
        close(fds[0]);
        test_result("pipe", strcmp(buf, msg) == 0);
    } else {
        test_result("pipe", 0);
    }
}

/* Test 6: open/read/close */
static void test_open(void) {
    int fd = open("/dev/null", 0);
    if (fd >= 0) {
        close(fd);
        test_result("open", 1);
    } else {
        test_result("open", 0);
    }
}

/* Test 7: clock_gettime */
static void test_clock(void) {
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    test_result("clock_gettime", rc == 0);
}

/* Test 8: getenv/setenv */
static void test_env(void) {
    setenv("M92_TEST_VAR", "musl_works", 1);
    char *val = getenv("M92_TEST_VAR");
    test_result("getenv+setenv", val && strcmp(val, "musl_works") == 0);
}

/* Test 9: pthread */
static void *thread_func(void *arg) { *(int*)arg = 123; return NULL; }

static void test_pthread(void) {
    pthread_t tid;
    int result = 0;
    if (pthread_create(&tid, NULL, thread_func, &result) == 0) {
        test_result("pthread", pthread_join(tid, NULL) == 0 && result == 123);
    } else {
        test_result("pthread", 0);
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    raw_out("M92-MUSL: start\n");

    /* Install SIGUSR1 handler early — musl's pthread_create may trigger
     * spurious SIGUSR1 delivery through the b1nix kernel. Without a handler
     * installed, the default action (kill) terminates the process. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sig_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
    }

    test_write();
    test_malloc();
    test_fork();
    test_pipe();
    test_open();
    test_clock();
    test_env();
    test_pthread();
    test_signal();

    raw_out("M92-MUSL: done (ok=");
    { char buf[8]; int n=0, v=ok_count; if(!v) buf[n++]='0'; while(v) { buf[n++]='0'+(v%10); v/=10; } for(int i=0;i<n;i++) write(1,&buf[n-1-i],1); }
    raw_out(" fail=");
    { char buf[8]; int n=0, v=fail_count; if(!v) buf[n++]='0'; while(v) { buf[n++]='0'+(v%10); v/=10; } for(int i=0;i<n;i++) write(1,&buf[n-1-i],1); }
    raw_out(")\n");

    return fail_count > 0 ? 1 : 0;
}
