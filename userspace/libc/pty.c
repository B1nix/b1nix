/* M32b — userspace pseudo-terminal helpers built on /dev/ptmx + /dev/pts/N. */
#include <pty.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

int posix_openpt(int flags) {
  return open("/dev/ptmx", flags);
}

int grantpt(int fd) {
  (void)fd; /* b1nix slaves carry no separate ownership to grant */
  return 0;
}

int unlockpt(int fd) {
  int unlock = 0;
  return ioctl(fd, TIOCSPTLCK, &unlock);
}

int ptsname_r(int fd, char *buf, size_t buflen) {
  unsigned int n = 0;
  if (ioctl(fd, TIOCGPTN, &n) < 0)
    return -1;
  snprintf(buf, buflen, "/dev/pts/%u", n);
  return 0;
}

char *ptsname(int fd) {
  static char buf[32];
  if (ptsname_r(fd, buf, sizeof(buf)) < 0)
    return 0;
  return buf;
}

void cfmakeraw(struct termios *t) {
  if (!t)
    return;
  t->c_iflag &= ~(ICRNL);
  t->c_oflag &= ~(OPOST | ONLCR);
  t->c_lflag &= ~(ECHO | ICANON | ISIG);
}

int openpty(int *amaster, int *aslave, char *name, const struct termios *termp,
            const struct winsize *winp) {
  int master = posix_openpt(O_RDWR);
  if (master < 0)
    return -1;
  if (grantpt(master) < 0 || unlockpt(master) < 0) {
    close(master);
    return -1;
  }
  char pts[32];
  if (ptsname_r(master, pts, sizeof(pts)) < 0) {
    close(master);
    return -1;
  }
  int slave = open(pts, O_RDWR);
  if (slave < 0) {
    close(master);
    return -1;
  }
  if (termp)
    tcsetattr(slave, TCSANOW, termp);
  if (winp)
    ioctl(slave, TIOCSWINSZ, (void *)winp);
  if (amaster)
    *amaster = master;
  if (aslave)
    *aslave = slave;
  if (name)
    strcpy(name, pts);
  return 0;
}

int login_tty(int fd) {
  setsid();
  /* TIOCSCTTY is best-effort (the slave may already be our controlling tty);
   * the stdio redirection must happen regardless. */
  ioctl(fd, TIOCSCTTY, 0);
  dup2(fd, 0);
  dup2(fd, 1);
  dup2(fd, 2);
  if (fd > 2)
    close(fd);
  return 0;
}

pid_t forkpty(int *amaster, char *name, const struct termios *termp,
              const struct winsize *winp) {
  int master, slave;
  if (openpty(&master, &slave, name, termp, winp) < 0)
    return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(master);
    close(slave);
    return -1;
  }
  if (pid == 0) {
    close(master);
    login_tty(slave);
    return 0;
  }
  close(slave);
  if (amaster)
    *amaster = master;
  return pid;
}
