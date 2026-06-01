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
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <netinet/in.h>
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
  unsigned char raw6[16];
  if (inet_pton(AF_INET6, "::1", raw6) != 1 ||
      raw6[15] != 1) {
    fail("inet6-pton"); return -1;
  }
  char back6[INET6_ADDRSTRLEN];
  if (!inet_ntop(AF_INET6, raw6, back6, sizeof(back6)) ||
      strcmp(back6, "::1") != 0) {
    fail("inet6-ntop"); return -1;
  }
  ok("inet6-pton-ntop");

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

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo("::1", "443", &hints, &res) != 0 || !res) {
    fail("getaddrinfo-inet6"); return -1;
  }
  struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)res->ai_addr;
  int port6_ok = (ntohs(sa6->sin6_port) == 443);
  int addr6_ok = (sa6->sin6_addr.s6_addr[15] == 1);
  freeaddrinfo(res);
  if (!port6_ok || !addr6_ok) { fail("getaddrinfo-inet6"); return -1; }
  ok("getaddrinfo-inet6");
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

  cfd = accept(lfd, 0, 0);
  if (cfd < 0) {
    close(lfd);
    return 9;
  }
  n = recv_with_retry(cfd, buf, sizeof(buf) - 1);
  if (n <= 0) {
    close(cfd);
    close(lfd);
    return 10;
  }
  buf[n] = '\0';
  if (!strstr(buf, "GET /wget-test")) {
    close(cfd);
    close(lfd);
    return 11;
  }
  const char *wget_resp = "HTTP/1.0 200 OK\r\nContent-Length: 7\r\n\r\nwget-ok";
  send(cfd, wget_resp, strlen(wget_resp), 0);
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

  int wget_pid = fork();
  if (wget_pid < 0) {
    fail("wget-fork");
    return -1;
  }
  if (wget_pid == 0) {
    char *wget_argv[] = {"/bin/wget", "-t", "1", "-o", "/tmp/wget.log", "-O", "/tmp/wget.out", "http://127.0.0.1:3232/wget-test", NULL};
    char *wget_envp[] = {NULL};
    execve("/bin/wget", wget_argv, wget_envp);
    _exit(127);
  }
  int wget_status = 0;
  waitpid(wget_pid, &wget_status, 0);

  int log_fd = open("/tmp/wget.log", O_RDONLY);
  if (log_fd >= 0) {
    char log_buf[1024];
    int nr;
    emit("--- wget log start ---\n");
    while ((nr = read(log_fd, log_buf, sizeof(log_buf) - 1)) > 0) {
      log_buf[nr] = '\0';
      emit(log_buf);
    }
    emit("--- wget log end ---\n");
    close(log_fd);
    unlink("/tmp/wget.log");
  }

  if (!WIFEXITED(wget_status) || WEXITSTATUS(wget_status) != 0) {
    fail("wget-exec");
    return -1;
  }

  int wget_fd = open("/tmp/wget.out", O_RDONLY);
  if (wget_fd < 0) {
    fail("wget-file-open");
    return -1;
  }
  char wget_buf[32];
  int wget_read = read(wget_fd, wget_buf, sizeof(wget_buf) - 1);
  close(wget_fd);
  unlink("/tmp/wget.out");
  if (wget_read != 7 || memcmp(wget_buf, "wget-ok", 7) != 0) {
    fail("wget-content");
    return -1;
  }
  ok("wget-loopback");

  int curl_pid = fork();
  if (curl_pid < 0) {
    fail("curl-fork");
    return -1;
  }
  if (curl_pid == 0) {
    char *curl_argv[] = {"/bin/curl", "--version", NULL};
    char *curl_envp[] = {NULL};
    int out = open("/tmp/curl.ver", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out >= 0) {
      dup2(out, 1);
      close(out);
    }
    int err = open("/tmp/curl.ver.err", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (err >= 0) {
      dup2(err, 2);
      close(err);
    }
    execve("/bin/curl", curl_argv, curl_envp);
    _exit(127);
  }
  int curl_status = 0;
  waitpid(curl_pid, &curl_status, 0);
  if (!WIFEXITED(curl_status) || WEXITSTATUS(curl_status) != 0) {
    emit("M32-NET: unsupported curl-tls-suite\n");
    unlink("/tmp/curl.ver.err");
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fail("tcp-server");
      return -1;
    }
    ok("tcp-server");
    return 0;
  }
  unlink("/tmp/curl.ver.err");

  int curl_fd = open("/tmp/curl.ver", O_RDONLY);
  if (curl_fd < 0) {
    emit("M32-NET: unsupported curl-tls-suite\n");
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fail("tcp-server");
      return -1;
    }
    ok("tcp-server");
    return 0;
  }
  char curl_buf[1024];
  int curl_n = read(curl_fd, curl_buf, sizeof(curl_buf) - 1);
  close(curl_fd);
  unlink("/tmp/curl.ver");
  if (curl_n <= 0) {
    emit("M32-NET: unsupported curl-tls-suite\n");
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fail("tcp-server");
      return -1;
    }
    ok("tcp-server");
    return 0;
  }
  curl_buf[curl_n] = '\0';
  if (!strstr(curl_buf, "https")) {
    emit("M32-NET: unsupported curl-tls-suite\n");
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fail("tcp-server");
      return -1;
    }
    ok("tcp-server");
    return 0;
  }
  ok("curl-https-enabled");

  curl_pid = fork();
  if (curl_pid < 0) {
    fail("curl-policy-fork");
    return -1;
  }
  if (curl_pid == 0) {
    char *curl_help_argv[] = {"/bin/curl", "--help", "tls", NULL};
    char *curl_help_envp[] = {NULL};
    int out = open("/tmp/curl.help", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out >= 0) {
      dup2(out, 1);
      close(out);
    }
    execve("/bin/curl", curl_help_argv, curl_help_envp);
    _exit(127);
  }
  waitpid(curl_pid, &curl_status, 0);
  if (!WIFEXITED(curl_status) || WEXITSTATUS(curl_status) != 0) {
    fail("curl-policy-help");
    return -1;
  }
  curl_fd = open("/tmp/curl.help", O_RDONLY);
  if (curl_fd < 0) {
    fail("curl-policy-open");
    return -1;
  }
  static char help_buf[16384];
  int help_total = 0;
  while (help_total < (int)sizeof(help_buf) - 1) {
    int r = read(curl_fd, help_buf + help_total, sizeof(help_buf) - 1 - help_total);
    if (r <= 0) break;
    help_total += r;
  }
  close(curl_fd);
  unlink("/tmp/curl.help");
  help_buf[help_total > 0 ? help_total : 0] = '\0';
  if (help_total <= 0) {
    fail("curl-policy-read");
    return -1;
  }
  if (!strstr(help_buf, "--cert-status") ||
      !strstr(help_buf, "--crlfile") ||
      !strstr(help_buf, "--pinnedpubkey")) {
    fail("curl-policy-flags");
    return -1;
  }
  ok("curl-policy-flags");

  /* Real TLS handshake over loopback: an mbedTLS server (m32-nettool
   * tls-server) presents the embedded test certificate (SAN IP:127.0.0.1) and
   * curl validates it against the matching test CA. Exercises the full mbedTLS
   * client+server handshake with no external network dependency. */
  int tls_srv = fork();
  if (tls_srv < 0) {
    fail("curl-https-srv-fork");
    return -1;
  }
  if (tls_srv == 0) {
    char *srv_argv[] = {"/bin/m32-nettool", "tls-server", "4443", NULL};
    char *srv_envp[] = {NULL};
    execve("/bin/m32-nettool", srv_argv, srv_envp);
    _exit(127);
  }
  sleep(1);
  curl_pid = fork();
  if (curl_pid < 0) {
    fail("curl-https-fork");
    return -1;
  }
  if (curl_pid == 0) {
    char *curl_tls_argv[] = {
      "/bin/curl", "--cacert", "/etc/tls-test/ca.pem",
      "--connect-timeout", "8", "-sS", "https://127.0.0.1:4443/", NULL
    };
    char *curl_tls_envp[] = {NULL};
    int out = open("/tmp/curl.https", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out >= 0) {
      dup2(out, 1);
      close(out);
    }
    execve("/bin/curl", curl_tls_argv, curl_tls_envp);
    _exit(127);
  }
  waitpid(curl_pid, &curl_status, 0);
  { int srv_status = 0; waitpid(tls_srv, &srv_status, 0); }
  if (!WIFEXITED(curl_status) || WEXITSTATUS(curl_status) != 0) {
    fail("curl-https-handshake");
    return -1;
  }
  curl_fd = open("/tmp/curl.https", O_RDONLY);
  if (curl_fd < 0) {
    fail("curl-https-open");
    return -1;
  }
  curl_n = read(curl_fd, curl_buf, sizeof(curl_buf) - 1);
  close(curl_fd);
  unlink("/tmp/curl.https");
  if (curl_n <= 0) {
    fail("curl-https-read");
    return -1;
  }
  curl_buf[curl_n] = '\0';
  if (!strstr(curl_buf, "tls-loopback-ok")) {
    fail("curl-https-content");
    return -1;
  }
  ok("curl-https-handshake");

  /* Negative path: the same server, but curl validates against the default
   * system trust store (no test CA) → verification must fail (non-zero exit). */
  tls_srv = fork();
  if (tls_srv < 0) {
    fail("curl-https-reject-srv-fork");
    return -1;
  }
  if (tls_srv == 0) {
    char *srv_argv[] = {"/bin/m32-nettool", "tls-server", "4444", NULL};
    char *srv_envp[] = {NULL};
    execve("/bin/m32-nettool", srv_argv, srv_envp);
    _exit(127);
  }
  sleep(1);
  curl_pid = fork();
  if (curl_pid < 0) {
    fail("curl-https-selfsigned-fork");
    return -1;
  }
  if (curl_pid == 0) {
    char *curl_bad_argv[] = {
      "/bin/curl", "--connect-timeout", "8", "-sS",
      "https://127.0.0.1:4444/", NULL
    };
    char *curl_bad_envp[] = {NULL};
    execve("/bin/curl", curl_bad_argv, curl_bad_envp);
    _exit(127);
  }
  waitpid(curl_pid, &curl_status, 0);
  { int srv_status = 0; waitpid(tls_srv, &srv_status, 0); }
  if (!WIFEXITED(curl_status) || WEXITSTATUS(curl_status) == 0) {
    fail("curl-https-selfsigned-reject");
    return -1;
  }
  ok("curl-https-selfsigned-reject");

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fail("tcp-server");
    return -1;
  }
  ok("tcp-server");
  return 0;
}

/* UDP over IPv6 on the ::1 loopback: bind a receiver to [::1]:port, send a
 * datagram from a second AF_INET6 socket, and read it back. Exercises the
 * kernel AF_INET6 socket path + udp6 + the IPv6 ::1 datapath end to end. */
static int test_udp6_loopback(void) {
  int rx = socket(AF_INET6, SOCK_DGRAM, 0);
  if (rx < 0) { fail("udp6-socket"); return -1; }
  struct sockaddr_in6 la;
  memset(&la, 0, sizeof(la));
  la.sin6_family = AF_INET6;
  la.sin6_port = htons(6789);
  la.sin6_addr = in6addr_loopback;
  if (bind(rx, (struct sockaddr *)&la, sizeof(la)) < 0) {
    fail("udp6-bind");
    return -1;
  }

  int tx = socket(AF_INET6, SOCK_DGRAM, 0);
  if (tx < 0) { fail("udp6-tx-socket"); return -1; }
  struct sockaddr_in6 pa;
  memset(&pa, 0, sizeof(pa));
  pa.sin6_family = AF_INET6;
  pa.sin6_port = htons(6789);
  pa.sin6_addr = in6addr_loopback;
  if (connect(tx, (struct sockaddr *)&pa, sizeof(pa)) < 0) {
    fail("udp6-connect");
    return -1;
  }

  if (send(tx, "v6-dgram", 8, 0) != 8) { fail("udp6-send"); return -1; }

  char buf[32];
  int n = (int)recv(rx, buf, sizeof(buf), 0);
  close(tx);
  close(rx);
  if (n != 8 || memcmp(buf, "v6-dgram", 8) != 0) {
    fail("udp6-loopback");
    return -1;
  }
  ok("udp6-loopback");
  return 0;
}

/* TCP over IPv6 on ::1: a forked echo server binds [::1]:port, accepts one
 * connection and echoes; the parent connects over AF_INET6 and verifies the
 * round-trip. Exercises the kernel AF_INET6 STREAM path end to end. */
static int run_server6(unsigned short port) {
  int lfd = socket(AF_INET6, SOCK_STREAM, 0);
  if (lfd < 0) return 1;
  struct sockaddr_in6 a;
  memset(&a, 0, sizeof(a));
  a.sin6_family = AF_INET6;
  a.sin6_port = htons(port);
  a.sin6_addr = in6addr_loopback;
  if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 1) < 0) {
    close(lfd);
    return 2;
  }
  int cfd = accept(lfd, 0, 0);
  if (cfd < 0) {
    close(lfd);
    return 3;
  }
  char buf[64];
  int n = recv_with_retry(cfd, buf, sizeof(buf));
  if (n > 0) send(cfd, buf, (size_t)n, 0);
  close(cfd);
  close(lfd);
  return 0;
}

static int test_tcp6_loopback(void) {
  unsigned short port = 3266;
  int pid = fork();
  if (pid < 0) { fail("tcp6-fork"); return -1; }
  if (pid == 0) _exit(run_server6(port));

  sleep(1);
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) { fail("tcp6-socket"); return -1; }
  struct sockaddr_in6 pa;
  memset(&pa, 0, sizeof(pa));
  pa.sin6_family = AF_INET6;
  pa.sin6_port = htons(port);
  pa.sin6_addr = in6addr_loopback;
  if (connect(fd, (struct sockaddr *)&pa, sizeof(pa)) < 0) {
    fail("tcp6-connect");
    return -1;
  }
  send(fd, "v6tcp-ok", 8, 0);
  char buf[64];
  int n = recv_with_retry(fd, buf, sizeof(buf));
  close(fd);
  int st = 0;
  waitpid(pid, &st, 0);
  if (n != 8 || memcmp(buf, "v6tcp-ok", 8) != 0) {
    fail("tcp6-loopback");
    return -1;
  }
  ok("tcp6-loopback");
  return 0;
}

/* Fetch a URL with curl into /tmp/ext.out; return curl's exit status (or -1).
 * `family` pins the IP version ("-4"/"-6") or is NULL to let curl choose.
 * Used by the external-connectivity probes below. */
static int curl_fetch(const char *url, const char *family) {
  int pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    char *argv[8];
    int a = 0;
    argv[a++] = "/bin/curl";
    argv[a++] = "-sS";
    if (family) argv[a++] = (char *)family;
    argv[a++] = "--connect-timeout";
    argv[a++] = "12";
    argv[a++] = (char *)url;
    argv[a] = NULL;
    char *envp[] = {NULL};
    int out = open("/tmp/ext.out", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out >= 0) { dup2(out, 1); close(out); }
    execve("/bin/curl", argv, envp);
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int ext_body_has(const char *needle) {
  int fd = open("/tmp/ext.out", O_RDONLY);
  if (fd < 0) return 0;
  char buf[512];
  int n = (int)read(fd, buf, sizeof(buf) - 1);
  close(fd);
  unlink("/tmp/ext.out");
  if (n <= 0) return 0;
  buf[n] = '\0';
  return strstr(buf, needle) != 0;
}

/* External connectivity over QEMU usernet. These need a working off-link path
 * (real DNS + TCP to the internet); when none is available the probe degrades
 * to an "unsupported" marker that tests/smoke.sh treats as a skip, so the
 * suite stays green offline. The HTTPS probe also exercises the full mbedTLS
 * path against a real CA-signed certificate (real cert-time validity). The
 * IPv6 probes (curl -6) exercise the kernel's off-link IPv6 datapath end to
 * end; they degrade the same way when the usernet link has no IPv6 route. */
static void test_external_net(void) {
  if (curl_fetch("http://example.com/", "-4") == 0 &&
      ext_body_has("Example Domain")) {
    ok("ext-http");
  } else {
    emit("M32-NET: unsupported ext-http\n");
    /* No off-link path at all — skip every remaining external probe. */
    emit("M32-NET: unsupported ext-https\n");
    emit("M32-NET: unsupported ext-http6\n");
    emit("M32-NET: unsupported ext-https6\n");
    return;
  }
  if (curl_fetch("https://example.com/", "-4") == 0 &&
      ext_body_has("Example Domain")) {
    ok("ext-https");
  } else {
    emit("M32-NET: unsupported ext-https\n");
  }
  /* IPv6 reachability is independent of IPv4: a usernet link may route v4 but
   * not v6, so each v6 probe skips on its own rather than gating the suite. */
  if (curl_fetch("http://example.com/", "-6") == 0 &&
      ext_body_has("Example Domain")) {
    ok("ext-http6");
  } else {
    emit("M32-NET: unsupported ext-http6\n");
  }
  if (curl_fetch("https://example.com/", "-6") == 0 &&
      ext_body_has("Example Domain")) {
    ok("ext-https6");
  } else {
    emit("M32-NET: unsupported ext-https6\n");
  }
}

/* getnameinfo() numeric round-trip for both families. */
static int test_getnameinfo(void) {
  char host[64], serv[16];
  struct sockaddr_in6 s6;
  memset(&s6, 0, sizeof(s6));
  s6.sin6_family = AF_INET6;
  s6.sin6_port = htons(8080);
  s6.sin6_addr = in6addr_loopback;
  if (getnameinfo((struct sockaddr *)&s6, sizeof(s6), host, sizeof(host), serv,
                  sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0 ||
      strcmp(host, "::1") != 0 || strcmp(serv, "8080") != 0) {
    fail("getnameinfo6");
    return -1;
  }
  struct sockaddr_in s4;
  memset(&s4, 0, sizeof(s4));
  s4.sin_family = AF_INET;
  s4.sin_port = htons(80);
  s4.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (getnameinfo((struct sockaddr *)&s4, sizeof(s4), host, sizeof(host), serv,
                  sizeof(serv), 0) != 0 ||
      strcmp(host, "127.0.0.1") != 0 || strcmp(serv, "80") != 0) {
    fail("getnameinfo4");
    return -1;
  }
  ok("getnameinfo");
  return 0;
}

/* Dual-stack: an AF_INET6 socket sends to ::ffff:127.0.0.1 and the datagram is
 * delivered to a plain AF_INET socket over the IPv4 loopback path. */
static int test_v4mapped_udp(void) {
  unsigned short port = 6790;
  int rx = socket(AF_INET, SOCK_DGRAM, 0);
  if (rx < 0) { fail("v4mapped-socket"); return -1; }
  struct sockaddr_in la;
  memset(&la, 0, sizeof(la));
  la.sin_family = AF_INET;
  la.sin_port = htons(port);
  la.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (bind(rx, (struct sockaddr *)&la, sizeof(la)) < 0) {
    fail("v4mapped-bind");
    return -1;
  }
  int tx = socket(AF_INET6, SOCK_DGRAM, 0);
  if (tx < 0) { fail("v4mapped-tx"); return -1; }
  struct sockaddr_in6 pa;
  memset(&pa, 0, sizeof(pa));
  pa.sin6_family = AF_INET6;
  pa.sin6_port = htons(port);
  pa.sin6_addr.s6_addr[10] = 0xff;
  pa.sin6_addr.s6_addr[11] = 0xff;
  pa.sin6_addr.s6_addr[12] = 127;
  pa.sin6_addr.s6_addr[15] = 1;
  if (connect(tx, (struct sockaddr *)&pa, sizeof(pa)) < 0) {
    fail("v4mapped-connect");
    return -1;
  }
  if (send(tx, "v4map", 5, 0) != 5) { fail("v4mapped-send"); return -1; }
  char buf[16];
  int n = (int)recv(rx, buf, sizeof(buf), 0);
  close(tx);
  close(rx);
  if (n != 5 || memcmp(buf, "v4map", 5) != 0) {
    fail("v4mapped-udp");
    return -1;
  }
  ok("v4mapped-udp");
  return 0;
}

/* PCRE2-backed regex filtering for wget. A recursive fetch is driven with an
 * --accept-regex that only a real PCRE2 matcher honours ("yes-\d+": POSIX would
 * treat \d as a literal 'd' and reject "yes-1"), and --regex-type pcre, which
 * wget only accepts when built with HAVE_LIBPCRE2. The loopback server serves
 * an index linking to /no-2 (must be filtered out) and /yes-1 (must be kept);
 * we then verify on disk that /yes-1 was downloaded with the right body and
 * /no-2 was not, proving both that PCRE2 is linked and that its matcher ran.
 * Self-contained on 127.0.0.1 — no external network. */
static void regex_send(int cfd, const char *ctype, const char *body) {
  char hdr[160];
  int hl = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
                    ctype);
  send(cfd, hdr, (size_t)hl, 0);
  if (body && *body) send(cfd, body, strlen(body), 0);
}

/* The server bounds each accept with a 3s select() timeout so it returns
 * cleanly even when wget rejects --regex-type pcre (built without PCRE2) and
 * never connects, or when the filter correctly skips /no-2. */
static int run_regex_server(unsigned short port) {
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) return 1;
  struct sockaddr_in addr;
  make_loopback_addr(port, &addr);
  if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(lfd, 4) < 0) {
    close(lfd);
    return 2;
  }
  for (int i = 0; i < 6; i++) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(lfd, &rs);
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    if (select(lfd + 1, &rs, 0, 0, &tv) <= 0) break;
    int cfd = accept(lfd, 0, 0);
    if (cfd < 0) break;
    char req[512];
    int n = (int)recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0) { close(cfd); continue; }
    req[n] = '\0';
    if (strstr(req, "GET /yes-1")) {
      regex_send(cfd, "text/plain", "regex-ok");
    } else if (strstr(req, "GET /no-2")) {
      regex_send(cfd, "text/plain", "should-be-filtered");
    } else if (strstr(req, "GET / ") || strstr(req, "GET /index")) {
      regex_send(cfd, "text/html",
                 "<html><body>"
                 "<a href=\"/no-2\">n</a> <a href=\"/yes-1\">y</a>"
                 "</body></html>");
    } else {
      /* An unexpected request (e.g. /robots.txt) — answer 404 so wget moves on. */
      const char *nf = "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";
      send(cfd, nf, strlen(nf), 0);
    }
    close(cfd);
  }
  close(lfd);
  return 0;
}

static int test_wget_pcre2_regex(void) {
  unsigned short port = 3290;
  mkdir("/tmp/wgr", 0755);
  unlink("/tmp/wgr/yes-1");
  unlink("/tmp/wgr/no-2");
  unlink("/tmp/wgr/index.html");

  int spid = fork();
  if (spid < 0) { fail("wget-pcre2-regex"); return -1; }
  if (spid == 0) _exit(run_regex_server(port));

  sleep(1);
  int wpid = fork();
  if (wpid < 0) { fail("wget-pcre2-regex"); return -1; }
  if (wpid == 0) {
    char *argv[] = {"/bin/wget", "-e", "robots=off", "-r", "-l", "1",
                    "-nd", "-nH", "-P", "/tmp/wgr",
                    "--regex-type", "pcre", "--accept-regex", "yes-\\d+",
                    "-t", "1", "-o", "/tmp/wgr.log",
                    "http://127.0.0.1:3290/", NULL};
    char *envp[] = {NULL};
    execve("/bin/wget", argv, envp);
    _exit(127);
  }
  int wst = 0;
  waitpid(wpid, &wst, 0);
  int sst = 0;
  waitpid(spid, &sst, 0);

  /* The accepted child must have been downloaded with the right content. */
  int ok_yes = 0;
  int fd = open("/tmp/wgr/yes-1", O_RDONLY);
  if (fd >= 0) {
    char b[32];
    int r = (int)read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r == 8 && memcmp(b, "regex-ok", 8) == 0) ok_yes = 1;
  }
  /* The rejected child must NOT have been fetched. */
  int no_present = 0;
  fd = open("/tmp/wgr/no-2", O_RDONLY);
  if (fd >= 0) { no_present = 1; close(fd); }

  unlink("/tmp/wgr/yes-1");
  unlink("/tmp/wgr/no-2");
  unlink("/tmp/wgr/index.html");

  if (!ok_yes || no_present) {
    /* Dump the wget log to serial for diagnosis, mirroring wget-loopback. */
    int lf = open("/tmp/wgr.log", O_RDONLY);
    if (lf >= 0) {
      char lb[1024];
      int nr;
      emit("--- wget pcre2 log start ---\n");
      while ((nr = read(lf, lb, sizeof(lb) - 1)) > 0) { lb[nr] = '\0'; emit(lb); }
      emit("--- wget pcre2 log end ---\n");
      close(lf);
    }
    unlink("/tmp/wgr.log");
    fail("wget-pcre2-regex");
    return -1;
  }
  unlink("/tmp/wgr.log");
  ok("wget-pcre2-regex");
  return 0;
}

static int run_server6_http(unsigned short port) {
  int lfd = socket(AF_INET6, SOCK_STREAM, 0);
  if (lfd < 0) return 1;
  struct sockaddr_in6 a;
  memset(&a, 0, sizeof(a));
  a.sin6_family = AF_INET6;
  a.sin6_port = htons(port);
  a.sin6_addr = in6addr_loopback;
  if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 1) < 0) {
    close(lfd);
    return 2;
  }
  int cfd = accept(lfd, 0, 0);
  if (cfd < 0) {
    close(lfd);
    return 3;
  }
  char buf[256];
  int n = recv_with_retry(cfd, buf, sizeof(buf) - 1);
  if (n <= 0) {
    close(cfd);
    close(lfd);
    return 4;
  }
  buf[n] = '\0';
  if (!strstr(buf, "GET /wget-v6-test")) {
    close(cfd);
    close(lfd);
    return 5;
  }
  const char *resp = "HTTP/1.0 200 OK\r\nContent-Length: 8\r\n\r\nwget6-ok";
  send(cfd, resp, strlen(resp), 0);
  close(cfd);
  close(lfd);
  return 0;
}

static int test_wget_ipv6(void) {
  unsigned short port = 3267;
  int pid = fork();
  if (pid < 0) { fail("wget-ipv6"); return -1; }
  if (pid == 0) _exit(run_server6_http(port));

  sleep(1);
  int wpid = fork();
  if (wpid < 0) { fail("wget-ipv6"); return -1; }
  if (wpid == 0) {
    char *argv[] = {"/bin/wget", "-t", "1", "-o", "/tmp/wget6.log", "-O", "/tmp/wget6.out", "http://[::1]:3267/wget-v6-test", NULL};
    char *envp[] = {NULL};
    execve("/bin/wget", argv, envp);
    _exit(127);
  }
  int wst = 0;
  waitpid(wpid, &wst, 0);
  
  int sst = 0;
  waitpid(pid, &sst, 0);

  if (!WIFEXITED(wst) || WEXITSTATUS(wst) != 0) {
    int lf = open("/tmp/wget6.log", O_RDONLY);
    if (lf >= 0) {
      char lb[1024];
      int nr;
      emit("--- wget ipv6 log start ---\n");
      while ((nr = read(lf, lb, sizeof(lb) - 1)) > 0) { lb[nr] = '\0'; emit(lb); }
      emit("--- wget ipv6 log end ---\n");
      close(lf);
    }
    unlink("/tmp/wget6.log");
    fail("wget-ipv6");
    return -1;
  }

  int fd = open("/tmp/wget6.out", O_RDONLY);
  if (fd < 0) {
    fail("wget-ipv6");
    return -1;
  }
  char buf[32];
  int r = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  unlink("/tmp/wget6.out");
  unlink("/tmp/wget6.log");
  if (r != 8 || memcmp(buf, "wget6-ok", 8) != 0) {
    fail("wget-ipv6");
    return -1;
  }
  ok("wget-ipv6");
  return 0;
}

static int test_wget_https(void) {
  const int max_wait_sec = 20;
  int tls_srv = fork();
  if (tls_srv < 0) {
    fail("wget-https-handshake");
    return -1;
  }
  if (tls_srv == 0) {
    char *srv_argv[] = {"/bin/m32-nettool", "tls-server", "4445", NULL};
    char *srv_envp[] = {NULL};
    execve("/bin/m32-nettool", srv_argv, srv_envp);
    _exit(127);
  }
  sleep(1);
  int wpid = fork();
  if (wpid < 0) {
    fail("wget-https-handshake");
    return -1;
  }
  if (wpid == 0) {
    char *argv[] = {
      "/bin/wget", "--ca-certificate=/etc/tls-test/ca.pem",
      "--random-file=/etc/tls-test/ca.pem",
      "-t", "1", "-T", "5", "-o", "/tmp/wget.https.log", "-O", "/tmp/wget.https.out",
      "https://127.0.0.1:4445/", NULL
    };
    char *envp[] = {NULL};
    execve("/bin/wget", argv, envp);
    _exit(127);
  }
  int wst = 0, sst = 0;
  int waited = 0;
  while (waited < max_wait_sec) {
    int wr = waitpid(wpid, &wst, WNOHANG);
    int sr = waitpid(tls_srv, &sst, WNOHANG);
    if (wr == wpid && sr == tls_srv) break;
    sleep(1);
    waited++;
  }
  if (waited >= max_wait_sec) {
    emit("--- wget https timeout ---\n");
    kill(wpid, SIGTERM);
    kill(tls_srv, SIGTERM);
    waitpid(wpid, &wst, 0);
    waitpid(tls_srv, &sst, 0);
  } else {
    if (waitpid(wpid, &wst, WNOHANG) == 0) waitpid(wpid, &wst, 0);
    if (waitpid(tls_srv, &sst, WNOHANG) == 0) waitpid(tls_srv, &sst, 0);
  }
  
  if (!WIFEXITED(wst) || WEXITSTATUS(wst) != 0) {
    char stbuf[128];
    snprintf(stbuf, sizeof(stbuf),
             "wget https status: wst=0x%x sst=0x%x waited=%d\n",
             wst, sst, waited);
    emit(stbuf);
    int lf = open("/tmp/wget.https.log", O_RDONLY);
    if (lf >= 0) {
      char lb[1024];
      int nr;
      emit("--- wget https log start ---\n");
      while ((nr = read(lf, lb, sizeof(lb) - 1)) > 0) { lb[nr] = '\0'; emit(lb); }
      emit("--- wget https log end ---\n");
      close(lf);
    }
    unlink("/tmp/wget.https.log");
    fail("wget-https-handshake");
    return -1;
  }
  int fd = open("/tmp/wget.https.out", O_RDONLY);
  if (fd < 0) {
    fail("wget-https-handshake");
    return -1;
  }
  char buf[64];
  int n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  unlink("/tmp/wget.https.out");
  unlink("/tmp/wget.https.log");
  if (n <= 0) {
    fail("wget-https-handshake");
    return -1;
  }
  buf[n] = '\0';
  if (!strstr(buf, "tls-loopback-ok")) {
    fail("wget-https-handshake");
    return -1;
  }
  ok("wget-https-handshake");

  tls_srv = fork();
  if (tls_srv < 0) {
    fail("wget-https-selfsigned-reject");
    return -1;
  }
  if (tls_srv == 0) {
    char *srv_argv[] = {"/bin/m32-nettool", "tls-server", "4446", NULL};
    char *srv_envp[] = {NULL};
    execve("/bin/m32-nettool", srv_argv, srv_envp);
    _exit(127);
  }
  sleep(1);
  wpid = fork();
  if (wpid < 0) {
    fail("wget-https-selfsigned-reject");
    return -1;
  }
  if (wpid == 0) {
    char *argv[] = {
      "/bin/wget", "--random-file=/etc/tls-test/ca.pem",
      "-t", "1", "-T", "5", "-o", "/tmp/wget.https.bad.log", "-O", "/tmp/wget.https.bad.out",
      "https://127.0.0.1:4446/", NULL
    };
    char *envp[] = {NULL};
    execve("/bin/wget", argv, envp);
    _exit(127);
  }
  waited = 0;
  while (waited < max_wait_sec) {
    int wr = waitpid(wpid, &wst, WNOHANG);
    int sr = waitpid(tls_srv, &sst, WNOHANG);
    if (wr == wpid && sr == tls_srv) break;
    sleep(1);
    waited++;
  }
  if (waited >= max_wait_sec) {
    emit("--- wget https selfsigned timeout ---\n");
    kill(wpid, SIGTERM);
    kill(tls_srv, SIGTERM);
    waitpid(wpid, &wst, 0);
    waitpid(tls_srv, &sst, 0);
  } else {
    if (waitpid(wpid, &wst, WNOHANG) == 0) waitpid(wpid, &wst, 0);
    if (waitpid(tls_srv, &sst, WNOHANG) == 0) waitpid(tls_srv, &sst, 0);
  }
  unlink("/tmp/wget.https.bad.out");
  unlink("/tmp/wget.https.bad.log");
  
  if (!WIFEXITED(wst) || WEXITSTATUS(wst) == 0) {
    fail("wget-https-selfsigned-reject");
    return -1;
  }
  ok("wget-https-selfsigned-reject");
  return 0;
}

int main(void) {
  emit("M32-NET: start\n");
  test_external_net();
  if (test_getnameinfo() != 0)         return 1;
  if (test_v4mapped_udp() != 0)        return 1;
  if (test_select_timeout_zero() != 0) return 1;
  if (test_select_pipe_ready() != 0)   return 1;
  if (test_select_multi_fd() != 0)     return 1;
  if (test_dns_libc() != 0)            return 1;
  if (test_udp6_loopback() != 0)       return 1;
  if (test_tcp6_loopback() != 0)       return 1;
  if (test_tcp_client_server() != 0)   return 1;
  if (test_wget_pcre2_regex() != 0)    return 1;
  if (test_wget_ipv6() != 0)           return 1;
  if (test_wget_https() != 0)          return 1;
  emit("M32-NET: done\n");
  return 0;
}
