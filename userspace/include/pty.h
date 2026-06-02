#ifndef B1NIX_U_PTY_H
#define B1NIX_U_PTY_H

#include <sys/types.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX pty master allocation */
int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);
int ptsname_r(int fd, char *buf, size_t buflen);

/* BSD-style convenience helpers (used by dropbear et al.) */
int openpty(int *amaster, int *aslave, char *name,
            const struct termios *termp, const struct winsize *winp);
pid_t forkpty(int *amaster, char *name, const struct termios *termp,
              const struct winsize *winp);

/* login_tty(fd): make fd the controlling terminal and wire it to stdio. */
int login_tty(int fd);

#ifdef __cplusplus
}
#endif

#endif
