#ifndef B1NIX_U_DIRENT_H
#define B1NIX_U_DIRENT_H

/* d_type values (Linux DT_*). readdir() fills d_type from the kernel entry
 * type; DT_UNKNOWN signals "stat() to find out". */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

/* ABI NOTE: d_name MUST stay at offset 8 (immediately after the 8-byte d_ino).
 * The cross-GCC libstdc++ and every ported C/C++ library (busybox, bash, curl,
 * fontconfig, freetype, ...) were compiled against the original 2-field layout
 * `{ unsigned long d_ino; char d_name[256]; }`, so they read d_name at offset 8.
 * d_type was added later (Chromium-port grind); placing it BEFORE d_name shifted
 * d_name to offset 9, so every prebuilt cross-GCC binary read the d_type byte as
 * d_name[0] — silently breaking readdir-based directory iteration (e.g. libstdc++
 * std::filesystem::directory_iterator never skipped "."/".." and Fontconfig found
 * zero fonts). Appending d_type after d_name keeps d_name at offset 8 for the
 * whole prebuilt ecosystem while still exposing d_type to freshly built code. */
struct dirent {
    unsigned long d_ino;
    char d_name[256];
    unsigned char d_type;
};

/* Opaque directory stream; backed by an open fd plus a batched SYS_GETDENTS
 * cursor (see libc/dirent.c). */
typedef struct __dirstream DIR;

#ifdef __cplusplus
extern "C" {
#endif

DIR *opendir(const char *name);
DIR *fdopendir(int fd);
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
