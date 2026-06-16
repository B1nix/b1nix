#ifndef B1NIX_U_DIRENT_H
#define B1NIX_U_DIRENT_H

struct dirent {
    unsigned long d_ino;
    char d_name[256];
};

/* Opaque directory stream; backed by an open fd plus a batched SYS_GETDENTS
 * cursor (see libc/dirent.c). */
typedef struct __dirstream DIR;

#ifdef __cplusplus
extern "C" {
#endif

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
int dirfd(DIR *dirp);
void rewinddir(DIR *dirp);
int scandir(const char *dir, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);

#ifdef __cplusplus
}
#endif

#ifndef HAVE_REWINDDIR
#define HAVE_REWINDDIR 1
#endif

#endif
