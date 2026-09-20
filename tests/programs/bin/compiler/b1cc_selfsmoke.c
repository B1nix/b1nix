/* b1cc_selfsmoke — M32/M33 on-device proof: /bin/b1cc, running as a B1NIX
 * process, compiles a C source AND links it with its own internal linker (no
 * host ld.lld / b1nix-cc), and the produced executable runs and exits 42.
 *
 * Mirrors m25_smoke: write source to /tmp, spawn the compiler,
 * spawn the output, check the exit status. On-device b1cc defaults to the
 * internal static linker and reads crt0.o + libb1nix.a from /lib/b1cc/.
 */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>

static void marker(const char *s) { write(1, s, strlen(s)); }

/* write a whole string to a fresh file; returns 0 on success */
static int write_file(const char *path, const char *contents) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t n = strlen(contents);
    int ok = (write(fd, contents, n) == (ssize_t)n) ? 0 : -1;
    close(fd);
    return ok;
}

static int run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execv(argv[0], argv); _exit(127); }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(void) {
    /* 1. write a trivial source (no #include, so no header deps) */

/* The compiler is asked for this machine's target explicitly: without it b1cc
 * falls back to its host default instead of the native b1nix path. */
#if defined(__aarch64__)
#define B1CC_TARGET_FLAG "--target=aarch64-b1nix"
#else
#define B1CC_TARGET_FLAG "--target=x86_64-b1nix"
#endif
    int fd = open("/tmp/b1cc_r42.c", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { marker("B1CC-SELF-COMPILE-SMOKE: fail write-src\n"); return 1; }
    const char *src = "int main(void) { return 40 + 2; }\n";
    write(fd, src, strlen(src));
    close(fd);

    /* 2. compile + internal-link with the on-device b1cc */
    char *cargv[] = { "/bin/b1cc", "/tmp/b1cc_r42.c",
                      B1CC_TARGET_FLAG, "-o", "/tmp/b1cc_r42", NULL };
    int crc = run(cargv);
    if (crc != 0) {
        char b[64];
        snprintf(b, sizeof b, "B1CC-SELF-COMPILE-SMOKE: fail compile rc=%d\n", crc);
        marker(b);
        return 1;
    }

    /* 3. run the produced binary — must exit 42 */
    char *rargv[] = { "/tmp/b1cc_r42", NULL };
    int rrc = run(rargv);
    if (rrc == 42) { marker("B1CC-SELF-COMPILE-SMOKE: ok\n"); }
    else {
        char b[64];
        snprintf(b, sizeof b, "B1CC-SELF-COMPILE-SMOKE: fail exit=%d\n", rrc);
        marker(b);
        return 1;
    }

    /* 4. M33 dynamic path: compile+link a PIE (ET_DYN) with b1cc's OWN internal
     * dynamic linker (crt0-dynamic.o + DT_NEEDED libc.so.1, no host ld.lld), and
     * run it under the kernel's eager in-kernel dynamic linker. The program
     * calls a libc function (exercises a PLT/JUMP_SLOT import) and returns 42. */
    int pfd = open("/tmp/b1cc_pie.c", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pfd < 0) { marker("B1CC-PIE-SMOKE: fail write-src\n"); return 1; }
    const char *psrc =
        "int puts(char *s);\n"
        "int main(void) { puts(\"b1cc pie on-device\"); return 42; }\n";
    write(pfd, psrc, strlen(psrc));
    close(pfd);

    char *pcargv[] = { "/bin/b1cc", "/tmp/b1cc_pie.c", "-fPIC",
                       B1CC_TARGET_FLAG, "-o", "/tmp/b1cc_pie", NULL };
    int pcrc = run(pcargv);
    if (pcrc != 0) {
        char b[64];
        snprintf(b, sizeof b, "B1CC-PIE-SMOKE: fail compile rc=%d\n", pcrc);
        marker(b);
        return 1;
    }

    char *prargv[] = { "/tmp/b1cc_pie", NULL };
    int prrc = run(prargv);
    if (prrc == 42) { marker("B1CC-PIE-SMOKE: ok\n"); }
    else {
        char b[64];
        snprintf(b, sizeof b, "B1CC-PIE-SMOKE: fail exit=%d\n", prrc);
        marker(b);
        return 1;
    }

    /* 5. M33 shared-object path: b1cc builds a .so (exporting add_three) AND a
     * separate PIE that DT_NEEDEDs it (via -lmath -> libmath.so.1), then runs the
     * PIE. The kernel loader must load /tmp/lib/libmath.so.1 through the
     * DT_NEEDED graph ($ORIGIN/../lib of /tmp/bin/dynmain) and bind the imported
     * add_three() out of the .so's exported dynsym. add_three(39) == 42. */
    mkdir("/tmp/bin", 0755);
    mkdir("/tmp/lib", 0755);
    if (write_file("/tmp/libmath.c", "int add_three(int x) { return x + 3; }\n") ||
        write_file("/tmp/dynmain.c",
                   "int add_three(int x);\n"
                   "int main(void) { return add_three(39); }\n")) {
        marker("B1CC-SO-SMOKE: fail write-src\n");
        return 1;
    }

    char *soargv[] = { "/bin/b1cc", "/tmp/libmath.c", "-fPIC", "-shared",
                       "--soname=libmath.so.1", B1CC_TARGET_FLAG,
                       "-o", "/tmp/lib/libmath.so.1", NULL };
    int sorc = run(soargv);
    if (sorc != 0) {
        char b[64];
        snprintf(b, sizeof b, "B1CC-SO-SMOKE: fail build-so rc=%d\n", sorc);
        marker(b);
        return 1;
    }

    char *mnargv[] = { "/bin/b1cc", "/tmp/dynmain.c", "-fPIC", "-lmath",
                       B1CC_TARGET_FLAG, "-o", "/tmp/bin/dynmain", NULL };
    int mnrc = run(mnargv);
    if (mnrc != 0) {
        char b[64];
        snprintf(b, sizeof b, "B1CC-SO-SMOKE: fail build-main rc=%d\n", mnrc);
        marker(b);
        return 1;
    }

    char *dmargv[] = { "/tmp/bin/dynmain", NULL };
    int dmrc = run(dmargv);
    if (dmrc == 42) { marker("B1CC-SO-SMOKE: ok\n"); return 0; }

    char b[64];
    snprintf(b, sizeof b, "B1CC-SO-SMOKE: fail exit=%d\n", dmrc);
    marker(b);
    return 1;
}
