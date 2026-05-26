#ifndef B1NIX_U_DIRENT_H
#define B1NIX_U_DIRENT_H

struct dirent {
    unsigned long d_ino;
    char d_name[256];
};

typedef struct {
    int fd;
} DIR;

static inline DIR *opendir(const char *name) {
    (void)name;
    return (DIR *)0;
}

static inline struct dirent *readdir(DIR *dirp) {
    (void)dirp;
    return (struct dirent *)0;
}

static inline int closedir(DIR *dirp) {
    (void)dirp;
    return 0;
}

#endif
