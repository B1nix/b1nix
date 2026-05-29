#ifndef B1NIX_U_DIRENT_H
#define B1NIX_U_DIRENT_H

struct dirent {
    unsigned long d_ino;
    char d_name[256];
};

/* Opaque directory stream; backed by an open fd plus a batched SYS_GETDENTS
 * cursor (see libc/dirent.c). */
typedef struct __dirstream DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
int dirfd(DIR *dirp);

#endif
