#ifndef B1NIX_SYS_SELECT_H
#define B1NIX_SYS_SELECT_H

#include <sys/types.h>
#include <sys/time.h>
/* fd_set and the FD_* macros live in <sys/types.h>. Ports (readline) reach
 * sigset_t through <sys/select.h> (for pselect-shaped prototypes), so pull the
 * signal types in here too. */
#include <signal.h>

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

#ifdef __cplusplus
extern "C" {
#endif

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#ifdef __cplusplus
}
#endif

#endif
