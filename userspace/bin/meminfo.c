#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        printf("meminfo: unable to open /proc/meminfo\n");
        return 1;
    }
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        fputs(buf, stdout);
    }
    fclose(f);
    return 0;
}
