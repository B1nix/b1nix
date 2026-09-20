/* Run the complete b1cc M34 differential corpus on B1NIX.  The source files
 * are embedded by the kernel initramfs generator; each member is compiled by
 * the on-device b1cc and then executed as a separate process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* The compiler is asked for this machine's target explicitly: without it b1cc
 * falls back to its host default instead of the native b1nix path. */
#if defined(__aarch64__)
#define B1CC_TARGET_FLAG "--target=aarch64-b1nix"
#else
#define B1CC_TARGET_FLAG "--target=x86_64-b1nix"
#endif

struct case_item { const char *name; const char *source; int expected; };
static const struct case_item cases[] = {
    {"return_42", "/tests/m34/return_42.c", 42},
    {"precedence", "/tests/m34/precedence.c", 14},
    {"local", "/tests/m34/local.c", 18},
    {"if_else", "/tests/m34/if_else.c", 7},
    {"while", "/tests/m34/while.c", 10},
    {"for", "/tests/m34/for.c", 15},
    {"function", "/tests/m34/function.c", 42},
    {"string_pointer", "/tests/m34/string_pointer.c", 10},
    {"m34_control_flow", "/tests/m34/m34_control_flow.c", 0},
    {"m34_expression_semantics", "/tests/m34/m34_expression_semantics.c", 0},
    {"m34_typedef_array_decay", "/tests/m34/m34_typedef_array_decay.c", 42},
    {"m34_local_partial_init", "/tests/m34/m34_local_partial_init.c", 42},
    {"m34_knr_params", "/tests/m34/m34_knr_params.c", 42},
    {"m34_const_correct", "/tests/m34/m34_const_correct.c", 42},
    {"m34_complex_storage", "/tests/m34/m34_complex_storage.c", 42},
    {"m34_vla_loop", "/tests/m34/m34_vla_loop.c", 42},
    {"m34_hideset_recursion", "/tests/m34/m34_hideset_recursion.c", 42},
    {"m34_va_copy", "/tests/m34/m34_va_copy.c", 42},
    {"m34_weak_symbol", "/tests/m34/m34_weak_symbol.c", 42},
    {"m34_headers", "/tests/m34/m34_headers.c", 42},
    {"m34_runtime", "/tests/m34/m34_runtime.c", 42},
    {"puts", "/tests/m34/puts.c", 10},
    {"m10_switch", "/tests/m34/m10_switch.c", 42},
    {"m11_globals", "/tests/m34/m11_globals.c", 42},
    {"m18_aggregates", "/tests/m34/m18_aggregates.c", 0},
};

static int run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execv(argv[0], argv); _exit(127); }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

int main(void) {
    int passed = 0;
    int total = (int)(sizeof(cases) / sizeof(cases[0]));
    setenv("M34VAR", "ok", 1);
    for (int i = 0; i < total; ++i) {
        char output[96];
        snprintf(output, sizeof(output), "/tmp/m34_%s", cases[i].name);
        char *compile_argv[] = {"/bin/b1cc", (char *)cases[i].source,
                                B1CC_TARGET_FLAG, "-o", output, "-lm", NULL};
        int compile_status = run(compile_argv);
        int status = -1;
        if (compile_status == 0) {
            if (strcmp(cases[i].name, "m34_runtime") == 0) {
                char *run_argv[] = {output, "one", "two", NULL};
                status = run(run_argv);
            } else {
                char *run_argv[] = {output, NULL};
                status = run(run_argv);
            }
        }
        if (compile_status == 0 && status == cases[i].expected) {
            printf("B1CC-M34-TARGET: %s ok\n", cases[i].name);
            passed++;
        } else {
            printf("B1CC-M34-TARGET: %s fail compile=%d exit=%d expected=%d\n",
                   cases[i].name, compile_status, status, cases[i].expected);
            if (strcmp(cases[i].name, "precedence") == 0) {
                char *dump_argv[] = {"/bin/b1cc", (char *)cases[i].source, B1CC_TARGET_FLAG, "-S", "-o", "/tmp/precedence.s", NULL};
                run(dump_argv);
                FILE *f = fopen("/tmp/precedence.s", "r");
                if (f) {
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        printf("DUMP: %s", line);
                    }
                    fclose(f);
                }
            }
        }
    }
    printf("B1CC-M34-TARGET: %d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
