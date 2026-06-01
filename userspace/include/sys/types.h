#ifndef B1NIX_SYS_TYPES_H
#define B1NIX_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
typedef long off_t;
#ifndef B1NIX_TIME_T_DEFINED
#define B1NIX_TIME_T_DEFINED
typedef long time_t;
#endif
typedef int pid_t;
typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned int nlink_t;

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif
#ifndef B1NIX_FD_SET_DEFINED
#define B1NIX_FD_SET_DEFINED
typedef struct {
  unsigned char bits[FD_SETSIZE / 8];
} fd_set;
#endif

#ifndef FD_ZERO
#define FD_ZERO(set)  do { \
    for (int _i = 0; _i < (int)(FD_SETSIZE / 8); _i++) (set)->bits[_i] = 0; \
  } while (0)
#endif
#ifndef FD_SET
#define FD_SET(fd, set)   ((set)->bits[(fd) / 8] |= (unsigned char)(1 << ((fd) & 7)))
#endif
#ifndef FD_CLR
#define FD_CLR(fd, set)   ((set)->bits[(fd) / 8] &= (unsigned char)~(1 << ((fd) & 7)))
#endif
#ifndef FD_ISSET
#define FD_ISSET(fd, set) (((set)->bits[(fd) / 8] >> ((fd) & 7)) & 1)
#endif

#endif
