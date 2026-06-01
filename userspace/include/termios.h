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

#define ECHO 0x00000008
#define ICANON 0x00000002
#define ISIG 0x00000001
#define OPOST 0x00000001
#define TOSTOP 0x00000100

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);

#ifdef __cplusplus
}
#endif

#endif
