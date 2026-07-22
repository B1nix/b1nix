#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: chmod <octal_mode> <file>\n");
        return 1;
    }
    
    const char *mode_str = argv[1];
    const char *file_path = argv[2];
    
    char *endptr;
    unsigned int mode = (unsigned int)strtol(mode_str, &endptr, 8);
    if (*endptr != '\0') {
        fprintf(stderr, "chmod: invalid mode '%s'\n", mode_str);
        return 1;
    }
    
    if (chmod(file_path, mode) < 0) {
        perror("chmod failed");
        return 1;
    }
    
    return 0;
}
