/* M13 Job Control Smoke Test
 * Written against POSIX APIs.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <sched.h>
#include <termios.h>
#include <stdlib.h>

static void marker(const char *s) { write(1, s, strlen(s)); }

static int wait_for_status(int pid, int options, int *status_out, int tries) {
  int st = 0;
  for (int i = 0; i < tries; i++) {
    long rc = waitpid(pid, &st, options | WNOHANG);
    if (rc == pid) {
      if (status_out)
        *status_out = st;
      return 0;
    }
    sched_yield();
    usleep(2000);
  }
  if (status_out)
    *status_out = st;
  return -1;
}

static int stopped_by(int status, int sig) {
  return WIFSTOPPED(status) && WSTOPSIG(status) == sig;
}

int main(void) {
  marker("M13-JC-SMOKE: start\n");

  int self_pid = getpid();
  if (self_pid <= 0) {
    marker("M13-JC-SMOKE: fail setup\n");
    return 1;
  }
  if (setpgid(0, 0) != 0) {
    marker("M13-JC-SMOKE: fail setup\n");
    return 1;
  }
  int self_pgrp = getpgrp();
  int old_fg = 0;
  if (self_pgrp <= 0 ||
      ioctl(0, TIOCGPGRP, &old_fg) != 0 || old_fg <= 0) {
    marker("M13-JC-SMOKE: fail setup\n");
    return 1;
  }

  int child = fork();
  if (child < 0) {
    marker("M13-JC-SMOKE: fail fork\n");
    return 1;
  }

  if (child == 0) {
    setpgid(0, 0);
    for (;;) {
      sched_yield();
    }
  }

  setpgid(child, child);
  int fg = child;
  int set_ok = -1;
  for (int i = 0; i < 64; i++) {
    set_ok = ioctl(0, TIOCSPGRP, &fg);
    if (set_ok == 0)
      break;
    sched_yield();
  }
  if (set_ok != 0) {
    marker("M13-JC-SMOKE: fail tcsetpgrp-child\n");
    return 1;
  }
  marker("M13-JC-SMOKE: ok tcsetpgrp-child\n");

  if (kill(child, SIGTSTP) != 0) {
    marker("M13-JC-SMOKE: fail sigtstp\n");
    return 1;
  }

  int st = 0;
  if (wait_for_status(child, WUNTRACED, &st, 256) != 0 ||
      !WIFSTOPPED(st)) {
    marker("M13-JC-SMOKE: fail wuntraced\n");
    return 1;
  }
  marker("M13-JC-SMOKE: ok wuntraced\n");

  fg = self_pgrp;
  if (ioctl(0, TIOCSPGRP, &fg) != 0) {
    marker("M13-JC-SMOKE: fail tcsetpgrp-self\n");
    return 1;
  }
  marker("M13-JC-SMOKE: ok tcsetpgrp-self\n");

  if (kill(child, SIGCONT) != 0) {
    marker("M13-JC-SMOKE: fail sigcont\n");
    return 1;
  }
  if (wait_for_status(child, WCONTINUED, &st, 128) != 0 ||
      !WIFCONTINUED(st)) {
    marker("M13-JC-SMOKE: fail wcontinued\n");
    return 1;
  }
  marker("M13-JC-SMOKE: ok wcontinued\n");

  kill(child, SIGKILL);
  waitpid(child, &st, 0);

  int reader = fork();
  if (reader < 0) {
    marker("M13-JC-SMOKE: fail sigttin\n");
    return 1;
  }
  if (reader == 0) {
    setpgid(0, 0);
    char c = 0;
    read(0, &c, 1);
    _exit(3);
  }
  setpgid(reader, reader);
  if (wait_for_status(reader, WUNTRACED, &st, 128) != 0 ||
      !stopped_by(st, SIGTTIN)) {
    marker("M13-JC-SMOKE: fail sigttin\n");
    kill(reader, SIGKILL);
    waitpid(reader, &st, 0);
    return 1;
  }
  marker("M13-JC-SMOKE: ok sigttin\n");
  kill(reader, SIGKILL);
  waitpid(reader, &st, 0);

  struct termios old_t;
  struct termios new_t;
  if (tcgetattr(1, &old_t) != 0) {
    marker("M13-JC-SMOKE: fail sigttou\n");
    return 1;
  }
  new_t = old_t;
  new_t.c_lflag |= TOSTOP;
  if (tcsetattr(1, TCSADRAIN, &new_t) != 0) {
    marker("M13-JC-SMOKE: fail sigttou\n");
    return 1;
  }

  int writer = fork();
  if (writer < 0) {
    tcsetattr(1, TCSADRAIN, &old_t);
    marker("M13-JC-SMOKE: fail sigttou\n");
    return 1;
  }
  if (writer == 0) {
    setpgid(0, 0);
    write(1, "x", 1);
    _exit(4);
  }
  setpgid(writer, writer);
  if (wait_for_status(writer, WUNTRACED, &st, 128) != 0 ||
      !stopped_by(st, SIGTTOU)) {
    tcsetattr(1, TCSADRAIN, &old_t);
    marker("M13-JC-SMOKE: fail sigttou\n");
    kill(writer, SIGKILL);
    waitpid(writer, &st, 0);
    return 1;
  }
  tcsetattr(1, TCSADRAIN, &old_t);
  marker("M13-JC-SMOKE: ok sigttou\n");
  kill(writer, SIGKILL);
  waitpid(writer, &st, 0);

  marker("M13-JC-SMOKE: done\n");
  return 0;
}
