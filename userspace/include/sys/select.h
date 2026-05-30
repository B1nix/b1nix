#ifndef B1NIX_SYS_SELECT_H
#define B1NIX_SYS_SELECT_H

#include <sys/types.h>

#define FD_SETSIZE 1024

typedef struct {
  unsigned char bits[FD_SETSIZE / 8];
} fd_set;

#define FD_ZERO(set)  do { \
    for (int _i = 0; _i < (int)(FD_SETSIZE / 8); _i++) (set)->bits[_i] = 0; \
  } while (0)
#define FD_SET(fd, set)   ((set)->bits[(fd) / 8] |= (unsigned char)(1 << ((fd) & 7)))
#define FD_CLR(fd, set)   ((set)->bits[(fd) / 8] &= (unsigned char)~(1 << ((fd) & 7)))
#define FD_ISSET(fd, set) (((set)->bits[(fd) / 8] >> ((fd) & 7)) & 1)

struct b1nix_timeval {
  long tv_sec;
  long tv_usec;
};
#define timeval b1nix_timeval

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#endif
