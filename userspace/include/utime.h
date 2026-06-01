#ifndef B1NIX_UTIME_H
#define B1NIX_UTIME_H

#include <sys/types.h>
#include <time.h>

struct utimbuf {
    time_t actime;
    time_t modtime;
};

int utime(const char *filename, const struct utimbuf *times);

#endif
