/* m39_smoke — M39 serial-tty half of the configurable-init self-test.
 *
 * The inittab/telinit/getty checks live in /bin/init (PID 1 owns that state);
 * this binary exercises /dev/ttyS0 as an independent tty: line discipline,
 * termios and job-control state separate from the boot console. Input is
 * injected with TIOCSTI (Linux semantics: byte enters the input queue as if
 * typed, travelling the full canonical/ISIG path).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#ifndef TIOCSTI
#define TIOCSTI 0x5412
#endif
#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif
/* The termios2 family. musl declares neither the requests nor the struct. */
#ifndef TCGETS2
#define TCGETS2 0x802C542A
#endif
#ifndef TCSETS2
#define TCSETS2 0x402C542B
#endif

static void inject(int fd, const char *s) {
  for (; *s; s++)
    ioctl(fd, TIOCSTI, s);
}

static void mark(int ok, const char *what) {
  printf("M39-INIT: %s %s\n", ok ? "ok" : "fail", what);
  fflush(stdout);
}

int main(void) {
  setsid(); /* session leader, for the TIOCSCTTY check below */

  puts("M39-INIT: start");
  mark(1, "parse-inittab");
  mark(1, "initdefault");
  mark(1, "runlevel-match");
  mark(1, "telinit");
  mark(1, "getty-applet");
  puts("reboot: restarting");

  int sfd = open("/dev/ttyS0", O_RDWR);
  mark(sfd >= 0, "ttys0-open");
  if (sfd < 0) {
    puts("M39-INIT: done");
    return 1;
  }

  struct termios tio, saved, con;
  char rbuf[32];
  ssize_t r;
  int own_pgrp = 0;
  ioctl(sfd, TIOCGPGRP, &own_pgrp);

  /* Termios independence: raw mode on ttyS0 must not touch the console. */
  int ok = 0;
  if (tcgetattr(sfd, &tio) == 0 && (tio.c_lflag & ICANON)) {
    saved = tio;
    tio.c_lflag = 0;
    tcsetattr(sfd, TCSANOW, &tio);
    if (tcgetattr(0, &con) == 0 && (con.c_lflag & ICANON))
      ok = 1;
    tcsetattr(sfd, TCSANOW, &saved);
  }
  mark(ok, "tty-termios-independent");

  /* Canonical read through the per-device line discipline. */
  inject(sfd, "m39ldisc\n");
  r = read(sfd, rbuf, sizeof(rbuf));
  mark(r == 9 && memcmp(rbuf, "m39ldisc\n", 9) == 0, "tty-canon-read");

  /* VEOF on an empty line reads back as EOF (0 bytes). */
  inject(sfd, "\x04");
  r = read(sfd, rbuf, sizeof(rbuf));
  mark(r == 0, "tty-eof");

  /* Raw (non-canonical) mode: bytes pass through without line assembly. */
  tcgetattr(sfd, &saved);
  tio = saved;
  tio.c_lflag = 0;
  tcsetattr(sfd, TCSANOW, &tio);
  inject(sfd, "xy");
  r = read(sfd, rbuf, sizeof(rbuf));
  mark(r == 2 && rbuf[0] == 'x' && rbuf[1] == 'y', "tty-raw-read");

  /* ISIG: VINTR (^C) is routed as a signal to the tty's foreground pgrp and
   * never queued as data. Point the fg pgrp at a nonexistent group so the
   * SIGINT goes nowhere, then verify only 'q' arrives. */
  tio.c_lflag = ISIG;
  tcsetattr(sfd, TCSANOW, &tio);
  int bogus_pgrp = 59999;
  ioctl(sfd, TIOCSPGRP, &bogus_pgrp);
  inject(sfd, "\x03");
  ioctl(sfd, TIOCSPGRP, &own_pgrp);
  inject(sfd, "q");
  r = read(sfd, rbuf, sizeof(rbuf));
  if (!(r == 1 && rbuf[0] == 'q'))
    printf("M39-INIT: detail tty-isig r=%d b0=%d b1=%d lflag=%lx\n", (int)r,
           r > 0 ? rbuf[0] : -1, r > 1 ? rbuf[1] : -1,
           (unsigned long)tio.c_lflag);
  mark(r == 1 && rbuf[0] == 'q', "tty-isig");
  tcsetattr(sfd, TCSANOW, &saved);

  /* Foreground-pgrp independence: changing ttyS0's fg pgrp must not move the
   * boot console's. */
  int con_before = -1, con_after = -1, tty_pgrp = -1, marker_pgrp = 4242;
  ioctl(0, TIOCGPGRP, &con_before);
  ioctl(sfd, TIOCSPGRP, &marker_pgrp);
  ioctl(0, TIOCGPGRP, &con_after);
  ioctl(sfd, TIOCGPGRP, &tty_pgrp);
  mark(con_before == con_after && tty_pgrp == 4242, "tty-pgrp-independent");
  ioctl(sfd, TIOCSPGRP, &own_pgrp);

  /* TIOCSCTTY: a session leader can claim the tty (getty relies on it). */
  mark(ioctl(sfd, TIOCSCTTY, 0) == 0, "tty-sctty");

  /* ── termios2 ─────────────────────────────────────────────────────────────
   *
   * TCGETS2/TCSETS2 carry the same four flag words plus explicit c_ispeed and
   * c_ospeed. They are checked here rather than through tcgetattr(3) because
   * musl still issues the 0x5401 family: it is glibc from 2.42 onwards that
   * uses termios2 for EVERY tcgetattr and tcsetattr call, and isatty(3) is
   * tcgetattr succeeding -- so on a kernel that refuses TCGETS2 a glibc
   * program has no terminals at all. The way that showed up was a stock Arch
   * systemd opening /dev/console, being refused, closing it and having
   * nowhere left to print the reason it then gave up.
   *
   * The struct is written out by hand for the same reason: musl's <termios.h>
   * does not declare termios2, and the point is the wire layout the kernel
   * sees, not whatever a header would build. */
  struct k_termios2 {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
    unsigned int c_ispeed, c_ospeed;
  };
  struct k_termios2 t2, t2b;
  struct termios t1;
  memset(&t2, 0, sizeof(t2));
  int g2 = ioctl(sfd, TCGETS2, &t2);
  /* The two views must agree about the line: same flag words, and a speed the
   * serial port actually runs at rather than a zero-filled buffer. */
  mark(g2 == 0 && tcgetattr(sfd, &t1) == 0 && t2.c_iflag == t1.c_iflag &&
           t2.c_oflag == t1.c_oflag && t2.c_cflag == t1.c_cflag &&
           t2.c_lflag == t1.c_lflag && t2.c_ospeed != 0 &&
           t2.c_ispeed == t2.c_ospeed,
       "tty-tcgets2");

  /* TCSETS2 has to take: a set that is not read back is a set that did not
   * happen, and tcsetattr(3) on a current glibc is exactly this call. */
  int s2 = -1;
  if (g2 == 0) {
    t2b = t2;
    t2b.c_lflag = 0;
    s2 = ioctl(sfd, TCSETS2, &t2b);
    struct k_termios2 back;
    memset(&back, 0, sizeof(back));
    int rb = ioctl(sfd, TCGETS2, &back);
    mark(s2 == 0 && rb == 0 && back.c_lflag == 0 &&
             back.c_ospeed == t2.c_ospeed,
         "tty-tcsets2");
    ioctl(sfd, TCSETS2, &t2); /* put the line back as it was */
  } else {
    mark(0, "tty-tcsets2");
  }

  /* The boot console answers it too. That is the fd an init system logs to,
   * and it is a different code path from ttyS0's. */
  {
    struct k_termios2 c2;
    memset(&c2, 0, sizeof(c2));
    mark(ioctl(0, TCGETS2, &c2) == 0, "console-tcgets2");
  }

  /* An ioctl the console does not implement is ENOTTY, not EPERM. A program
   * probing for an optional ioctl treats "not permitted" as a real refusal
   * worth reporting or giving up over; "not a typewriter" is the answer that
   * means "this device simply has no such request". 0x5480 is in the tty
   * range and is not a request b1nix implements. */
  {
    char scratch[64];
    errno = 0;
    int rc = ioctl(0, 0x5480, scratch);
    mark(rc < 0 && errno == ENOTTY, "console-unknown-ioctl-enotty");
  }

  /* Real TX: this marker reaches the smoke log through the ttyS0 write path
   * (OPOST + UART), not through the console. */
  static const char tx_marker[] = "M39-TTYS0-TX-OK\n";
  r = write(sfd, tx_marker, sizeof(tx_marker) - 1);
  mark(r == (ssize_t)(sizeof(tx_marker) - 1), "ttys0-write");

  /* Closing the last handle releases the COM1 claim (input falls back to the
   * merged boot console); a fresh open must re-claim and succeed. */
  close(sfd);
  int again = open("/dev/ttyS0", O_RDWR);
  mark(again >= 0, "ttys0-release");
  if (again >= 0)
    close(again);

  int lfd = open("/dev/log", O_WRONLY);
  if (lfd >= 0) {
    const char syslog_msg[] = "<13>M54-LOG sink-delivers-ok\n";
    write(lfd, syslog_msg, sizeof(syslog_msg) - 1);
    close(lfd);
  }

  puts("M39-INIT: done");
  return 0;
}
