#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("--- B1NIX Native Compiler Test ---\n");
    char *buf = malloc(128);
    if (!buf) {
        printf("Malloc failed!\n");
        return 1;
    }
    sprintf(buf, "Native compilation works! %d + %d = %d", 20, 22, 42);
    puts(buf);
    return 0;
}
