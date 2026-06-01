#ifndef B1NIX_U_SYS_RESOURCE_H
#define B1NIX_U_SYS_RESOURCE_H

#include <sys/types.h>

typedef unsigned long rlim_t;

struct rlimit {
  rlim_t rlim_cur;
  rlim_t rlim_max;
};

#define RLIMIT_NOFILE 7
#define RLIM_INFINITY ((rlim_t)-1)

int getrlimit(int resource, struct rlimit *rlim);

#endif
