/*
 * M92: Minimal musl test — just write + exit. No malloc, no printf, no fork.
 * Tests the absolute minimum: can musl's _start → __libc_start_main work?
 */
#include <unistd.h>

static int slen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    const char *msg = "M92-MUSL: hello from musl\n";
    write(1, msg, slen(msg));
    return 0;
}
