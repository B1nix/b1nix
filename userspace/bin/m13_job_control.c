#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>

#define B1NIX_SIGKILL 10
#define B1NIX_SIGTSTP 16
#define B1NIX_SIGTTIN 17
#define B1NIX_SIGTTOU 18
#define B1NIX_SIGCONT 5

#define B1NIX_WNOHANG 1
#define B1NIX_WUNTRACED 2
#define B1NIX_WCONTINUED 8

#define B1NIX_TCGETS 0x5401
#define B1NIX_TCSETS 0x5402
#define B1NIX_TIOCGPGRP 0x540F
#define B1NIX_TIOCSPGRP 0x5410
#define B1NIX_TOSTOP 0x00000100

struct smoke_termios {
  unsigned int c_iflag;
  unsigned int c_oflag;
  unsigned int c_cflag;
  unsigned int c_lflag;
  unsigned char c_cc[32];
};

static void marker(const char *s) { write(1, s, strlen(s)); }

static int wait_for_status(int pid, int options, int *status_out, int tries) {
  int st = 0;
  for (int i = 0; i < tries; i++) {
    long rc = syscall(SYS_WAITPID, pid, &st, options | B1NIX_WNOHANG);
    if (rc == pid) {
      if (status_out)
        *status_out = st;
      return 0;
    }
    syscall(SYS_YIELD);
  }
  return -1;
}

static int stopped_by(int status, int sig) {
  return (status & 0xFF) == 0x7F && ((status >> 8) & 0xFF) == sig;
}

int main(void) {
  marker("M13-JC-SMOKE: start\n");

  int self_pid = (int)syscall(SYS_GETPID);
  if (self_pid <= 0) {
    marker("M13-JC-SMOKE: fail setup\n");
    return 1;
  }
  if ((int)syscall(SYS_SETPGRP, 0, 0) != 0) {
    marker("M13-JC-SMOKE: fail setup\n");
    return 1;
  }
  int self_pgrp = (int)syscall(SYS_GETPGRP);
  unsigned long old_fg = 0;
  if (self_pgrp <= 0 ||
      (int)syscall(SYS_IOCTL, 0, B1NIX_TIOCGPGRP, &old_fg) != 0 || old_fg <= 0) {
    marker("M13-JC-SMOKE: fail setup\n");
    return 1;
  }

  int child = (int)syscall(SYS_FORK);
  if (child < 0) {
    marker("M13-JC-SMOKE: fail fork\n");
    return 1;
  }

  if (child == 0) {
    syscall(SYS_SETPGRP, 0, 0);
    for (;;) {
      syscall(SYS_YIELD);
    }
  }

  unsigned long fg = (unsigned long)child;
  (void)syscall(SYS_SETPGRP, child, child);
  int set_ok = -1;
  for (int i = 0; i < 64; i++) {
    set_ok = (int)syscall(SYS_IOCTL, 0, B1NIX_TIOCSPGRP, &fg);
    if (set_ok == 0)
      break;
    syscall(SYS_YIELD);
  }
  if (set_ok != 0) {
    marker("M13-JC-SMOKE: fail tcsetpgrp-child\n");
    return 1;
  }
  marker("M13-JC-SMOKE: ok tcsetpgrp-child\n");

  if ((int)syscall(SYS_KILL, child, B1NIX_SIGTSTP) != 0) {
    marker("M13-JC-SMOKE: fail sigtstp\n");
    return 1;
  }

  int st = 0;
  if (wait_for_status(child, B1NIX_WUNTRACED, &st, 128) != 0 ||
      (st & 0xFF) != 0x7F) {
    marker("M13-JC-SMOKE: fail wuntraced\n");
    return 1;
  }
  marker("M13-JC-SMOKE: ok wuntraced\n");

  fg = (unsigned long)self_pgrp;
  if ((int)syscall(SYS_IOCTL, 0, B1NIX_TIOCSPGRP, &fg) != 0) {
    marker("M13-JC-SMOKE: fail tcsetpgrp-self\n");
    return 1;
  }
  marker("M13-JC-SMOKE: ok tcsetpgrp-self\n");

  if ((int)syscall(SYS_KILL, child, B1NIX_SIGCONT) != 0) {
    marker("M13-JC-SMOKE: fail sigcont\n");
    return 1;
  }
  if (wait_for_status(child, B1NIX_WCONTINUED, &st, 128) != 0 ||
      st != 0xFFFF) {
    marker("M13-JC-SMOKE: fail wcontinued\n");
    return 1;
  }
  marker("M13-JC-SMOKE: ok wcontinued\n");

  syscall(SYS_KILL, child, B1NIX_SIGKILL);
  syscall(SYS_WAITPID, child, &st, 0);

  int reader = (int)syscall(SYS_FORK);
  if (reader < 0) {
    marker("M13-JC-SMOKE: fail sigttin\n");
    return 1;
  }
  if (reader == 0) {
    syscall(SYS_SETPGRP, 0, 0);
    char c = 0;
    syscall(SYS_READ, 0, &c, 1);
    syscall(SYS_EXIT, 3);
  }
  (void)syscall(SYS_SETPGRP, reader, reader);
  if (wait_for_status(reader, B1NIX_WUNTRACED, &st, 128) != 0 ||
      !stopped_by(st, B1NIX_SIGTTIN)) {
    marker("M13-JC-SMOKE: fail sigttin\n");
    syscall(SYS_KILL, reader, B1NIX_SIGKILL);
    syscall(SYS_WAITPID, reader, &st, 0);
    return 1;
  }
  marker("M13-JC-SMOKE: ok sigttin\n");
  syscall(SYS_KILL, reader, B1NIX_SIGKILL);
  syscall(SYS_WAITPID, reader, &st, 0);

  struct smoke_termios old_t;
  struct smoke_termios new_t;
  if ((int)syscall(SYS_IOCTL, 1, B1NIX_TCGETS, &old_t) != 0) {
    marker("M13-JC-SMOKE: fail sigttou\n");
    return 1;
  }
  new_t = old_t;
  new_t.c_lflag |= B1NIX_TOSTOP;
  if ((int)syscall(SYS_IOCTL, 1, B1NIX_TCSETS, &new_t) != 0) {
    marker("M13-JC-SMOKE: fail sigttou\n");
    return 1;
  }

  int writer = (int)syscall(SYS_FORK);
  if (writer < 0) {
    syscall(SYS_IOCTL, 1, B1NIX_TCSETS, &old_t);
    marker("M13-JC-SMOKE: fail sigttou\n");
    return 1;
  }
  if (writer == 0) {
    syscall(SYS_SETPGRP, 0, 0);
    syscall(SYS_WRITE, 1, "x", 1);
    syscall(SYS_EXIT, 4);
  }
  (void)syscall(SYS_SETPGRP, writer, writer);
  if (wait_for_status(writer, B1NIX_WUNTRACED, &st, 128) != 0 ||
      !stopped_by(st, B1NIX_SIGTTOU)) {
    syscall(SYS_IOCTL, 1, B1NIX_TCSETS, &old_t);
    marker("M13-JC-SMOKE: fail sigttou\n");
    syscall(SYS_KILL, writer, B1NIX_SIGKILL);
    syscall(SYS_WAITPID, writer, &st, 0);
    return 1;
  }
  syscall(SYS_IOCTL, 1, B1NIX_TCSETS, &old_t);
  marker("M13-JC-SMOKE: ok sigttou\n");
  syscall(SYS_KILL, writer, B1NIX_SIGKILL);
  syscall(SYS_WAITPID, writer, &st, 0);

  marker("M13-JC-SMOKE: done\n");
  return 0;
}
