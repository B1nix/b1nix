#ifndef B1NIX_SYS_TYPES_H
#define B1NIX_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
typedef long long off_t;
#ifndef B1NIX_TIME_T_DEFINED
#define B1NIX_TIME_T_DEFINED
typedef long long time_t;
#endif
typedef int pid_t;
typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned int nlink_t;
typedef long blksize_t;
typedef long blkcnt_t;
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
typedef long suseconds_t;
typedef unsigned int useconds_t;
typedef long clock_t;

/* BSD-style aliases expected by older portable C (dropbear et al.). */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char uchar;
typedef unsigned int uint;

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
