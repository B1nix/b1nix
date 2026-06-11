#ifndef B1NIX_U_SYS_RESOURCE_H
#define B1NIX_U_SYS_RESOURCE_H

#include <sys/types.h>

/* getpriority/setpriority `which` values (PRIO_PROCESS only is honored). */
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

typedef unsigned long rlim_t;

struct rlimit {
  rlim_t rlim_cur;
  rlim_t rlim_max;
};

#define RLIMIT_CPU    0
#define RLIMIT_FSIZE  1
#define RLIMIT_DATA   2
#define RLIMIT_STACK  3
#define RLIMIT_CORE   4
#define RLIMIT_NOFILE 7
#define RLIMIT_AS     9
#define RLIM_INFINITY ((rlim_t)-1)

#ifdef __cplusplus
extern "C" {
#endif

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);

#ifdef __cplusplus
}
#endif

#endif
