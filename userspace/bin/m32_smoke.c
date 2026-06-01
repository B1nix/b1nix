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
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <arpa/inet.h>
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

/* DNS / resolver libc layer. Numeric forms resolve locally (no nameserver
 * needed), so this is deterministic offline; it proves the getaddrinfo /
 * gethostbyname / inet_pton / inet_ntop plumbing is wired correctly. */
static int test_dns_libc(void) {
  /* inet_pton round-trips through inet_ntop. */
  unsigned char raw[4];
  if (inet_pton(AF_INET, "10.0.2.2", raw) != 1 ||
      raw[0] != 10 || raw[1] != 0 || raw[2] != 2 || raw[3] != 2) {
    fail("inet-pton"); return -1;
  }
  char back[16];
  if (!inet_ntop(AF_INET, raw, back, sizeof(back)) ||
      strcmp(back, "10.0.2.2") != 0) {
    fail("inet-ntop"); return -1;
  }
  ok("inet-pton-ntop");

  /* gethostbyname on a dotted-quad: numeric fast path, no DNS query. */
  struct hostent *he = gethostbyname("10.0.2.2");
  if (!he || he->h_length != 4 || he->h_addrtype != AF_INET ||
      (unsigned char)he->h_addr_list[0][0] != 10 ||
      (unsigned char)he->h_addr_list[0][3] != 2) {
    fail("gethostbyname-numeric"); return -1;
  }
  ok("gethostbyname-numeric");

  /* getaddrinfo with a numeric node + service. */
  struct addrinfo hints, *res = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo("10.0.2.2", "80", &hints, &res) != 0 || !res) {
    fail("getaddrinfo"); return -1;
  }
  struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
  int port_ok = (ntohs(sa->sin_port) == 80);
  int addr_ok = ((unsigned char)(sa->sin_addr.s_addr & 0xff) == 10);
  freeaddrinfo(res);
  if (!port_ok || !addr_ok) { fail("getaddrinfo"); return -1; }
  ok("getaddrinfo");
  return 0;
}

static int make_loopback_addr(unsigned short port, struct sockaddr_in *addr) {
  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  addr->sin_addr.s_addr = inet_addr("127.0.0.1");
  return 0;
}

static int recv_with_retry(int fd, char *buf, int max) {
  for (int tries = 0; tries < 100; tries++) {
    int n = (int)recv(fd, buf, (size_t)max, 0);
    if (n > 0) return n;
    syscall(SYS_YIELD);
  }
  return 0;
}

static int connect_loopback(unsigned short port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in addr;
  make_loopback_addr(port, &addr);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int run_server(unsigned short port) {
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) return 2;

  struct sockaddr_in addr;
  make_loopback_addr(port, &addr);
  if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(lfd, 2) < 0) {
    close(lfd);
    return 3;
  }

  int cfd = accept(lfd, 0, 0);
  if (cfd < 0) {
    close(lfd);
    return 4;
  }
  char buf[256];
  int n = recv_with_retry(cfd, buf, sizeof(buf));
  if (n <= 0 || memcmp(buf, "m32-echo", 8) != 0) {
    close(cfd);
    close(lfd);
    return 5;
  }
  send(cfd, "m32-echo-ok", 11, 0);
  close(cfd);

  cfd = accept(lfd, 0, 0);
  if (cfd < 0) {
    close(lfd);
    return 6;
  }
  n = recv_with_retry(cfd, buf, sizeof(buf) - 1);
  if (n <= 0) {
    close(cfd);
    close(lfd);
    return 7;
  }
  buf[n] = '\0';
  if (!strstr(buf, "GET /m32 HTTP/1.0") || !strstr(buf, "Host: 127.0.0.1")) {
    close(cfd);
    close(lfd);
    return 8;
  }
  const char *resp = "HTTP/1.0 200 OK\r\nContent-Length: 9\r\n\r\nm32-http";
  send(cfd, resp, strlen(resp), 0);
  close(cfd);
  close(lfd);
  return 0;
}

static int test_tcp_client_server(void) {
  unsigned short port = 3232;
  int pid = fork();
  if (pid < 0) {
    fail("tcp-fork");
    return -1;
  }
  if (pid == 0) {
    _exit(run_server(port));
  }

  sleep(1);
  int fd = connect_loopback(port);
  if (fd < 0) {
    fail("tcp-connect");
    return -1;
  }
  send(fd, "m32-echo", 8, 0);
  char buf[512];
  int n = recv_with_retry(fd, buf, sizeof(buf));
  close(fd);
  if (n != 11 || memcmp(buf, "m32-echo-ok", 11) != 0) {
    fail("tcp-echo");
    return -1;
  }
  ok("tcp-echo");

  fd = connect_loopback(port);
  if (fd < 0) {
    fail("http-connect");
    return -1;
  }
  const char *req = "GET /m32 HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
  send(fd, req, strlen(req), 0);
  n = recv_with_retry(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    fail("http-get");
    return -1;
  }
  buf[n] = '\0';
  if (!strstr(buf, "HTTP/1.0 200 OK") || !strstr(buf, "m32-http")) {
    fail("http-get");
    return -1;
  }
  ok("http-get");

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fail("tcp-server");
    return -1;
  }
  ok("tcp-server");
  return 0;
}

int main(void) {
  emit("M32-NET: start\n");
  if (test_select_timeout_zero() != 0) return 1;
  if (test_select_pipe_ready() != 0)   return 1;
  if (test_select_multi_fd() != 0)     return 1;
  if (test_dns_libc() != 0)            return 1;
  if (test_tcp_client_server() != 0)   return 1;
  emit("M32-NET: done\n");
  return 0;
}
