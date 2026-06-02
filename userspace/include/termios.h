#ifndef B1NIX_U_TERMIOS_H
#define B1NIX_U_TERMIOS_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_cc[32];
};

/* c_lflag */
#define ECHO 0x00000008
#define ICANON 0x00000002
#define ISIG 0x00000001
#define TOSTOP 0x00000100
/* c_oflag */
#define OPOST 0x00000001
#define ONLCR 0x00000004
/* c_iflag */
#define ICRNL 0x00000100

/* c_cc indices */
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VEOF   4
#define VSUSP  10

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* terminal ioctls */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCNOTTY  0x5422
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431

struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
void cfmakeraw(struct termios *termios_p);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);

#ifdef __cplusplus
}
#endif

#endif
