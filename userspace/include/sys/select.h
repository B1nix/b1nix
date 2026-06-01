#ifndef B1NIX_SYS_SELECT_H
#define B1NIX_SYS_SELECT_H

#include <sys/types.h>

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

#ifndef B1NIX_FD_SET_DEFINED
#define B1NIX_FD_SET_DEFINED
typedef struct {
  unsigned char bits[FD_SETSIZE / 8];
} fd_set;
#endif

struct b1nix_timeval {
  long tv_sec;
  long tv_usec;
};
#define timeval b1nix_timeval

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#endif
