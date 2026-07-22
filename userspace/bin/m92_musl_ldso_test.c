/*
 * M92-LDSO: userspace ld.so smoke test (ldso-migration-and-unix-parity-plan.md).
 *
 * This binary is linked as PIE/ET_DYN with a genuine PT_INTERP =
 * /lib/ld-musl-x86_64.so.1. The kernel loads only the interpreter's own
 * segments (unrelocated) and jumps to its entry point; musl's real ld.so
 * then self-relocates, loads/links this binary itself via ordinary
 * open/mmap syscalls, and jumps to main — with NO help from the kernel's
 * in-kernel eager linker.
 *
 * Tests: write, malloc, fork+waitpid, environ/getenv (dynamic-linker-only
 * state musl's crt/ldso set up, not the eager loader).
 */
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

static int ok_count = 0;
static int fail_count = 0;

static void raw_out(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
}

static void test_result(const char *name, int pass) {
    if (pass) {
        ok_count++;
        raw_out("M92-LDSO: ok ");
    } else {
        fail_count++;
        raw_out("M92-LDSO: fail ");
    }
    raw_out(name);
    raw_out("\n");
}

static void test_write(void) {
    const char *msg = "M92-LDSO: hello via real userspace ld.so\n";
    ssize_t n = write(STDOUT_FILENO, msg, strlen(msg));
    test_result("write", n > 0);
}

static void test_malloc(void) {
    void *p = malloc(1024);
    if (p) {
        memset(p, 0xAB, 1024);
        free(p);
        test_result("malloc", 1);
    } else {
        test_result("malloc", 0);
    }
}

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

static void test_getenv(void) {
    setenv("M92_LDSO_VAR", "present", 1);
    const char *v = getenv("M92_LDSO_VAR");
    test_result("getenv", v && strcmp(v, "present") == 0);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    raw_out("M92-LDSO: start\n");

    test_write();
    test_malloc();
    test_fork();
    test_getenv();

    raw_out("M92-LDSO: done (ok=");
    { char buf[8]; int n=0, v=ok_count; if(!v) buf[n++]='0'; while(v) { buf[n++]='0'+(v%10); v/=10; } for(int i=0;i<n;i++) write(1,&buf[n-1-i],1); }
    raw_out(" fail=");
    { char buf[8]; int n=0, v=fail_count; if(!v) buf[n++]='0'; while(v) { buf[n++]='0'+(v%10); v/=10; } for(int i=0;i<n;i++) write(1,&buf[n-1-i],1); }
    raw_out(")\n");

    return fail_count > 0 ? 1 : 0;
}
