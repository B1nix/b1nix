/*
 * M92: musl dynamic linking smoke test — verifies the kernel's in-kernel
 * eager dynamic linker can load musl's libc.so and resolve symbols.
 *
 * This binary is linked as PIE/ET_DYN against ld-musl-x86_64.so.1.
 * The kernel loads it, resolves DT_NEEDED, and applies relocations before
 * userspace starts.
 *
 * Tests: write, malloc, fork+waitpid (basic libc functionality via dynamic linking).
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
        raw_out("M92-MUSL-DYN: ok ");
    } else {
        fail_count++;
        raw_out("M92-MUSL-DYN: fail ");
    }
    raw_out(name);
    raw_out("\n");
}

/* Test 1: write — basic libc function via shared library */
static void test_write(void) {
    const char *msg = "M92-MUSL-DYN: hello from dynamic musl\n";
    ssize_t n = write(STDOUT_FILENO, msg, strlen(msg));
    test_result("write", n > 0);
}

/* Test 2: malloc/free — heap allocation via shared library */
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

/* Test 3: fork+waitpid — process creation via shared library */
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

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    raw_out("M92-MUSL-DYN: start\n");

    test_write();
    test_malloc();
    test_fork();

    raw_out("M92-MUSL-DYN: done (ok=");
    { char buf[8]; int n=0, v=ok_count; if(!v) buf[n++]='0'; while(v) { buf[n++]='0'+(v%10); v/=10; } for(int i=0;i<n;i++) write(1,&buf[n-1-i],1); }
    raw_out(" fail=");
    { char buf[8]; int n=0, v=fail_count; if(!v) buf[n++]='0'; while(v) { buf[n++]='0'+(v%10); v/=10; } for(int i=0;i<n;i++) write(1,&buf[n-1-i],1); }
    raw_out(")\n");

    return fail_count > 0 ? 1 : 0;
}
