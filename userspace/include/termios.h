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

/* c_iflag (Linux-compatible values) */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IUCLC   0x0200
#define IXON    0x0400
#define IXANY   0x0800
#define IXOFF   0x1000
#define IMAXBEL 0x2000
#define IUTF8   0x4000

/* c_oflag */
#define OPOST  0x0001
#define OLCUC  0x0002
#define ONLCR  0x0004
#define OCRNL  0x0008
#define ONOCR  0x0010
#define ONLRET 0x0020
#define OFILL  0x0040
#define OFDEL  0x0080

/* c_cflag */
#define CSIZE  0x0030
#define CS5    0x0000
#define CS6    0x0010
#define CS7    0x0020
#define CS8    0x0030
#define CSTOPB 0x0040
#define CREAD  0x0080
#define PARENB 0x0100
#define PARODD 0x0200
#define HUPCL  0x0400
#define CLOCAL 0x0800
#define CBAUD  0x100f

/* baud rates */
#define B0      0x0000
#define B50     0x0001
#define B75     0x0002
#define B110    0x0003
#define B134    0x0004
#define B150    0x0005
#define B200    0x0006
#define B300    0x0007
#define B600    0x0008
#define B1200   0x0009
#define B1800   0x000a
#define B2400   0x000b
#define B4800   0x000c
#define B9600   0x000d
#define B19200  0x000e
#define B38400  0x000f
#define B57600  0x1001
#define B115200 0x1002
#define B230400 0x1003

/* c_lflag */
#define ISIG    0x0001
#define ICANON  0x0002
#define XCASE   0x0004
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define ECHOCTL 0x0200
#define ECHOPRT 0x0400
#define ECHOKE  0x0800
#define FLUSHO  0x1000
#define PENDIN  0x4000
#define IEXTEN  0x8000
#define EXTPROC 0x10000

/* c_cc indices */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16
#define NCCS     32

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* tcflush queue selectors / tcflow actions */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

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

typedef unsigned int speed_t;

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
void cfmakeraw(struct termios *termios_p);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
pid_t tcgetsid(int fd);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int cfsetispeed(struct termios *t, speed_t speed);
int cfsetospeed(struct termios *t, speed_t speed);
int cfsetspeed(struct termios *t, speed_t speed);
int tcflush(int fd, int queue_selector);
int tcdrain(int fd);
int tcflow(int fd, int action);
int tcsendbreak(int fd, int duration);

#ifdef __cplusplus
}
#endif

#endif
