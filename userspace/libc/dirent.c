#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "syscall.h"

/* Kernel SYS_GETDENTS entry ABI. Must match kernel/include/b1nix/dirent.h
 * exactly (same field order/types => same x86_64 layout, sizeof 88). The
 * kernel fills an array of these; we translate to the POSIX `struct dirent`
 * that callers (e.g. GNU Make's dir.c) expect. */
struct k_dirent {
    char               name[64];
    unsigned int       type;
    unsigned int       is_dir;
    unsigned int       is_exec;
    unsigned long long size;
};

/* SYS_GETDENTS caps a single call at 32 entries kernel-side. */
#define DIRENT_BATCH 32

struct __dirstream {
    int               fd;
    int               count;   /* valid entries currently in batch */
    int               index;   /* next entry to hand out */
    int               eof;     /* getdents has returned 0 */
    unsigned long     ino_seq; /* synthetic, monotonically increasing d_ino */
    struct k_dirent   batch[DIRENT_BATCH];
    struct dirent     ent;     /* storage for the pointer returned by readdir */
};

DIR *opendir(const char *name)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0)
        return (DIR *)0;

    DIR *d = (DIR *)malloc(sizeof(struct __dirstream));
    if (!d) {
        close(fd);
        return (DIR *)0;
    }
    d->fd = fd;
    d->count = 0;
    d->index = 0;
    d->eof = 0;
    d->ino_seq = 0;
    return d;
}

struct dirent *readdir(DIR *dirp)
{
    if (!dirp)
        return (struct dirent *)0;

    if (dirp->index >= dirp->count) {
        if (dirp->eof)
            return (struct dirent *)0;
        long r = syscall(SYS_GETDENTS, dirp->fd, dirp->batch, DIRENT_BATCH);
        if (r <= 0) {
            dirp->eof = 1;
            return (struct dirent *)0;
        }
        dirp->count = (int)r;
        dirp->index = 0;
    }

    struct k_dirent *k = &dirp->batch[dirp->index++];
    dirp->ent.d_ino = ++dirp->ino_seq;

    int i = 0;
    while (i < (int)sizeof(k->name) - 1 && k->name[i]) {
        dirp->ent.d_name[i] = k->name[i];
        i++;
    }
    dirp->ent.d_name[i] = '\0';
    return &dirp->ent;
}

int closedir(DIR *dirp)
{
    if (!dirp)
        return -1;
    close(dirp->fd);
    free(dirp);
    return 0;
}

int dirfd(DIR *dirp)
{
    return dirp ? dirp->fd : -1;
}

void rewinddir(DIR *dirp)
{
    if (!dirp)
        return;
    lseek(dirp->fd, 0, 0);
    dirp->count = 0;
    dirp->index = 0;
    dirp->eof = 0;
}

/* POSIX scandir(3): read every entry of `dir`, keep the ones `filter` accepts
 * (or all if filter==NULL), sort them with `compar`, and return a malloc'd
 * array of malloc'd struct dirent* via *namelist. Returns the count, or -1 on
 * error (with the partial array freed). */
int scandir(const char *dir, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **))
{
    if (!dir || !namelist)
        return -1;

    DIR *d = opendir(dir);
    if (!d)
        return -1;

    struct dirent **list = NULL;
    size_t count = 0, cap = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (filter && !filter(ent))
            continue;
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            struct dirent **nl = realloc(list, ncap * sizeof(*list));
            if (!nl)
                goto fail;
            list = nl;
            cap = ncap;
        }
        struct dirent *copy = malloc(sizeof(struct dirent));
        if (!copy)
            goto fail;
        *copy = *ent;
        list[count++] = copy;
    }
    closedir(d);

    if (count > 1 && compar)
        qsort(list, count, sizeof(*list),
              (int (*)(const void *, const void *))compar);

    *namelist = list;
    return (int)count;

fail:
    for (size_t i = 0; i < count; i++)
        free(list[i]);
    free(list);
    closedir(d);
    return -1;
}

/* POSIX alphasort(3): compare two scandir entries by name. */
int alphasort(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}
