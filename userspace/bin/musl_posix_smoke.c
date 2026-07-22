/*
 * Simple POSIX smoke test for musl libc — verifies the build pipeline works.
 * Uses only standard POSIX APIs, no b1nix-specific syscalls.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
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

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    marker("MUSL-POSIX: start\n");
    test_write();
    test_malloc();
    test_fork();
    test_getenv();
    marker("MUSL-POSIX: done\n");
    return 0;
}
