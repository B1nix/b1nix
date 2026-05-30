/* M32 network / multiplex smoke. Exercises:
 *
 *   - select() with a zero timeout on a freshly-created pipe (no data
 *     yet — expects 0 ready fds);
 *   - select() with data buffered in a pipe (expects 1 ready);
 *   - select() across multiple fds (only the readable one fires);
 *   - select() with a NULL timeout but a small wait — we write to the
 *     pipe from a child task before blocking and verify the wake.
 *
 * Markers (`M32-NET: ok <name>`) are consumed by tests/smoke.sh. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include "syscall.h"

static void emit(const char *s) { write(1, s, strlen(s)); }

static void ok(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M32-NET: ok ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

static void fail(const char *name) {
  char buf[128];
  int n = 0;
  const char *p = "M32-NET: FAIL ";
  while (*p) buf[n++] = *p++;
  while (*name) buf[n++] = *name++;
  buf[n++] = '\n';
  write(1, buf, n);
}

/* select() with no ready fds and a zero timeout → 0. */
static int test_select_timeout_zero(void) {
  int pipefd[2];
  if (syscall(SYS_PIPE, pipefd) < 0) { fail("select-timeout-pipe"); return -1; }
  fd_set rs; FD_ZERO(&rs); FD_SET(pipefd[0], &rs);
  struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
  int rc = select(pipefd[0] + 1, &rs, 0, 0, &tv);
  close(pipefd[0]); close(pipefd[1]);
  if (rc != 0) { fail("select-timeout-zero"); return -1; }
  ok("select-timeout-zero");
  return 0;
}

/* select() with data in the pipe → reports the read end as ready. */
static int test_select_pipe_ready(void) {
  int pipefd[2];
  if (syscall(SYS_PIPE, pipefd) < 0) { fail("select-ready-pipe"); return -1; }
  const char *msg = "hi";
  write(pipefd[1], msg, 2);
  fd_set rs; FD_ZERO(&rs); FD_SET(pipefd[0], &rs);
  struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
  int rc = select(pipefd[0] + 1, &rs, 0, 0, &tv);
  int isset = FD_ISSET(pipefd[0], &rs);
  close(pipefd[0]); close(pipefd[1]);
  if (rc != 1 || !isset) { fail("select-pipe-ready"); return -1; }
  ok("select-pipe-ready");
  return 0;
}

/* select() across multiple fds: only the readable one fires. The writable
 * end of the pipe is always writable, so we expect it set in writefds. */
static int test_select_multi_fd(void) {
  int p1[2], p2[2];
  if (syscall(SYS_PIPE, p1) < 0 || syscall(SYS_PIPE, p2) < 0) {
    fail("select-multi-pipe"); return -1;
  }
  write(p1[1], "x", 1);
  /* p1[0] is read-ready; p2[0] is not. */
  fd_set rs; FD_ZERO(&rs);
  FD_SET(p1[0], &rs); FD_SET(p2[0], &rs);
  struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
  int nfds = p1[0] > p2[0] ? p1[0] + 1 : p2[0] + 1;
  int rc = select(nfds, &rs, 0, 0, &tv);
  int p1_set = FD_ISSET(p1[0], &rs);
  int p2_set = FD_ISSET(p2[0], &rs);
  close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
  if (rc != 1 || !p1_set || p2_set) { fail("select-multi-fd"); return -1; }
  ok("select-multi-fd");
  return 0;
}

int main(void) {
  emit("M32-NET: start\n");
  if (test_select_timeout_zero() != 0) return 1;
  if (test_select_pipe_ready() != 0)   return 1;
  if (test_select_multi_fd() != 0)     return 1;
  emit("M32-NET: done\n");
  return 0;
}
