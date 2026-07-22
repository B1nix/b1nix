/*
 * M92: Step 2 musl test — write + printf + malloc. No fork, no pthread.
 * Tests: write syscall, musl stdio formatting, musl malloc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    const char *msg = "M92-MUSL-STEP2: start\n";
    write(1, msg, strlen(msg));

    /* Test 1: printf */
    printf("M92-MUSL-STEP2: printf test %d\n", 42);

    /* Test 2: malloc */
    void *p = malloc(64);
    if (p) {
        memset(p, 0xAA, 64);
        printf("M92-MUSL-STEP2: malloc ok %p\n", p);
        free(p);
    } else {
        printf("M92-MUSL-STEP2: malloc FAIL\n");
    }

    printf("M92-MUSL-STEP2: done\n");
    return 0;
}
