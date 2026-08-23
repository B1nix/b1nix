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

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <pty.h>
#include <crypt.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <utmp.h>

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
  if (pipe(pipefd) < 0) { fail("select-timeout-pipe"); return -1; }
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
  if (pipe(pipefd) < 0) { fail("select-ready-pipe"); return -1; }
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
  if (pipe(p1) < 0 || pipe(p2) < 0) {
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

/* Bounded receive: wait for readability with a deadline, then recv() once.
 * A plain blocking recv() is POSIX-correct, but every test in this instance
 * runs sequentially under one init — so a single lost loopback datagram or a
 * peer that never answers stalls the WHOLE instance until the host watchdog
 * kills QEMU 120s later, losing every marker that would have come after.
 * Returning 0 on timeout makes such a case a FAIL of this one check instead.
 * Returns the byte count, 0 on timeout/EOF, -1 on error. */
static int recv_deadline(int fd, char *buf, int max, int timeout_ms) {
  struct pollfd p;
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  int rc = poll(&p, 1, timeout_ms);
  if (rc < 0) return -1;
  if (rc == 0) return 0;
  return (int)recv(fd, buf, (size_t)max, 0);
}

#define RECV_TIMEOUT_MS 15000

static int recv_with_retry(int fd, char *buf, int max) {
  int n = recv_deadline(fd, buf, max, RECV_TIMEOUT_MS);
  return n > 0 ? n : 0;
}

static int connect_loopback(unsigned short port) {
  struct sockaddr_in addr;
  make_loopback_addr(port, &addr);
  for (int tries = 0; tries < 100; tries++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      return fd;
    close(fd);
    sched_yield();
  }
  return -1;
}

/* Bounded accept: same rationale as recv_deadline — a server child stuck in
 * an unbounded accept() keeps the parent's final waitpid() from ever
 * returning, wedging the instance. Returns -1 on timeout. */
static int accept_deadline(int lfd, int timeout_ms) {
  struct pollfd p;
  p.fd = lfd;
  p.events = POLLIN;
  p.revents = 0;
  if (poll(&p, 1, timeout_ms) <= 0)
    return -1;
  return accept(lfd, 0, 0);
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

  int cfd = accept_deadline(lfd, RECV_TIMEOUT_MS);
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

  cfd = accept_deadline(lfd, RECV_TIMEOUT_MS);
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

  cfd = accept_deadline(lfd, RECV_TIMEOUT_MS);
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
  if (!strstr(buf, "GET /curl-test")) {
    close(cfd);
    close(lfd);
    return 11;
  }
  const char *client_resp = "HTTP/1.0 200 OK\r\nContent-Length: 7\r\n\r\ncurl-ok";
  send(cfd, client_resp, strlen(client_resp), 0);
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

  /* Same GET, this time through the real HTTP client: proves the loopback
   * datapath end to end for a full-size program (connect, request, response
   * parsing, file write) rather than the raw socket calls above. */
  int dl_pid = fork();
  if (dl_pid < 0) {
    fail("curl-fork");
    return -1;
  }
  if (dl_pid == 0) {
    char *dl_argv[] = {"/bin/curl", "-s", "-S", "-o", "/tmp/curl.out",
                         "http://127.0.0.1:3232/curl-test", NULL};
    char *dl_envp[] = {NULL};
    execve("/bin/curl", dl_argv, dl_envp);
    _exit(127);
  }
  int dl_status = 0;
  waitpid(dl_pid, &dl_status, 0);

  if (!WIFEXITED(dl_status) || WEXITSTATUS(dl_status) != 0) {
    fail("curl-exec");
    return -1;
  }

  int dl_fd = open("/tmp/curl.out", O_RDONLY);
  if (dl_fd < 0) {
    fail("curl-file-open");
    return -1;
  }
  char dl_buf[32];
  int dl_read = read(dl_fd, dl_buf, sizeof(dl_buf) - 1);
  close(dl_fd);
  unlink("/tmp/curl.out");
  if (dl_read != 7 || memcmp(dl_buf, "curl-ok", 7) != 0) {
    fail("curl-content");
    return -1;
  }
  ok("curl-loopback");

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
    char *srv_argv[] = {"/bin/m32_nettool", "tls-server", "4443", NULL};
    char *srv_envp[] = {NULL};
    execve("/bin/m32_nettool", srv_argv, srv_envp);
    _exit(127);
  }
  /* Retry the connect: under host load (the parallel smoke) the server's
   * fork+execve+bind can take longer than a fixed sleep, so a too-early
   * connect() is refused. The tls-server is single-shot but binds+listens
   * before its (slow) TLS init, so only the first SUCCESSFUL connect is served —
   * earlier refused attempts don't consume it. */
  curl_status = -1;
  for (int attempt = 0; attempt < 4; attempt++) {
    sleep(1);
    curl_pid = fork();
    if (curl_pid < 0) {
      fail("curl-https-fork");
      kill(tls_srv, SIGKILL);
      { int s = 0; waitpid(tls_srv, &s, 0); }
      return -1;
    }
    if (curl_pid == 0) {
      char *curl_tls_argv[] = {
        "/bin/curl", "--cacert", "/etc/tls-test/ca.pem",
        "--connect-timeout", "2", "-sS", "https://127.0.0.1:4443/", NULL
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
    if (WIFEXITED(curl_status) && WEXITSTATUS(curl_status) == 0)
      break;
  }
  /* Always terminate the server before reaping it: if every connect failed the
   * single-shot server is still blocked in accept(), so a plain waitpid() would
   * hang forever — the historical M32-NET smoke stall under parallel load.
   *
   * SIGKILL, not SIGTERM: the instance's test runner starts with
   * `trap '' ... TERM ...` (tools/ports/00-smoke.start) and SIG_IGN is
   * inherited across both fork and execve, so every process in the test tree
   * ignores SIGTERM. The kill silently did nothing, the waitpid below never
   * returned, and the lane died there — taking every check that runs after
   * m32_smoke (m53_*, m34, m35, m36, cxx, m55, m98, m104: ~380 of them) with
   * it. That is the whole 1022-vs-640 swing between runs. */
  kill(tls_srv, SIGKILL);
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
    char *srv_argv[] = {"/bin/m32_nettool", "tls-server", "4444", NULL};
    char *srv_envp[] = {NULL};
    execve("/bin/m32_nettool", srv_argv, srv_envp);
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
      "/bin/curl", "--connect-timeout", "2", "-sS",
      "https://127.0.0.1:4444/", NULL
    };
    char *curl_bad_envp[] = {NULL};
    execve("/bin/curl", curl_bad_argv, curl_bad_envp);
    _exit(127);
  }
  waitpid(curl_pid, &curl_status, 0);
  /* Same single-shot-server hang guard as the positive path: terminate before
   * reaping so a refused connect (server not yet bound under load) can't leave
   * us blocked in waitpid. SIGKILL for the same reason as above: the test tree
   * inherits an ignored SIGTERM from the runner's trap. */
  kill(tls_srv, SIGKILL);
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
  int n = recv_deadline(rx, buf, sizeof(buf), RECV_TIMEOUT_MS);
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

  struct sockaddr_in6 pa;
  memset(&pa, 0, sizeof(pa));
  pa.sin6_family = AF_INET6;
  pa.sin6_port = htons(port);
  pa.sin6_addr = in6addr_loopback;
  int fd = -1;
  for (int tries = 0; tries < 100; tries++) {
    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { fail("tcp6-socket"); return -1; }
    if (connect(fd, (struct sockaddr *)&pa, sizeof(pa)) == 0)
      break;
    close(fd);
    fd = -1;
    sched_yield();
  }
  if (fd < 0) {
    kill(pid, 9);
    int st = 0;
    waitpid(pid, &st, 0);
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
    char *argv[10];
    int a = 0;
    argv[a++] = "/bin/curl";
    argv[a++] = "-sS";
    if (family) argv[a++] = (char *)family;
    argv[a++] = "--connect-timeout";
    argv[a++] = "2";
    argv[a++] = "--max-time";
    argv[a++] = "3";
    argv[a++] = (char *)url;
    argv[a] = NULL;
    char *envp[] = {NULL};
    int out = open("/tmp/ext.out", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out >= 0) { dup2(out, 1); close(out); }
    execve("/bin/curl", argv, envp);
    _exit(127);
  }
  /* Plain blocking wait. A WNOHANG poll loop with usleep() was tried here to
   * cap a hung probe, and it made the lane twice as slow (246 s -> 478 s,
   * reproduced): the stuck curl is spinning in userspace, so the poller
   * competes with it for the one CPU this arch has instead of letting it run
   * out. The stall this was meant to bound is a real defect — see
   * docs/aarch64-parity.md — and it belongs in the kernel, not here. */
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
                  sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0 ||
      strcmp(host, "127.0.0.1") != 0 || strcmp(serv, "80") != 0) {
    fail("getnameinfo4");
    return -1;
  }
  /* Without NI_NUMERICHOST the resolver is allowed to name the address, and
   * now that reverse lookups work it does: /etc/hosts maps 127.0.0.1 to
   * localhost. Either answer is correct here — what must hold is that the
   * call succeeds and returns something. (This assertion used to demand the
   * numeric form, which only held while reverse resolution was broken.) */
  if (getnameinfo((struct sockaddr *)&s4, sizeof(s4), host, sizeof(host), serv,
                  sizeof(serv), 0) != 0 ||
      host[0] == 0 || serv[0] == 0) {
    fail("getnameinfo4-named");
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
  int n = recv_deadline(rx, buf, sizeof(buf), RECV_TIMEOUT_MS);
  close(tx);
  close(rx);
  if (n != 5 || memcmp(buf, "v4map", 5) != 0) {
    fail("v4mapped-udp");
    return -1;
  }
  ok("v4mapped-udp");
  return 0;
}

static int test_ipv6_v6only(void) {
  int fd = socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd < 0) { fail("ipv6-v6only-socket"); return -1; }
  int one = 1;
  if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) != 0) {
    fail("ipv6-v6only-set"); close(fd); return -1;
  }
  int value = 0;
  socklen_t len = sizeof(value);
  if (getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &value, &len) != 0 ||
      value != 1) {
    fail("ipv6-v6only-get"); close(fd); return -1;
  }
  struct sockaddr_in6 mapped;
  memset(&mapped, 0, sizeof(mapped));
  mapped.sin6_family = AF_INET6;
  mapped.sin6_port = htons(6791);
  mapped.sin6_addr.s6_addr[10] = 0xff;
  mapped.sin6_addr.s6_addr[11] = 0xff;
  mapped.sin6_addr.s6_addr[12] = 127;
  mapped.sin6_addr.s6_addr[15] = 1;
  if (connect(fd, (struct sockaddr *)&mapped, sizeof(mapped)) == 0) {
    fail("ipv6-v6only-mapped"); close(fd); return -1;
  }
  close(fd);
  ok("ipv6-v6only");
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
  if (!strstr(buf, "GET /curl-v6-test")) {
    close(cfd);
    close(lfd);
    return 5;
  }
  const char *resp = "HTTP/1.0 200 OK\r\nContent-Length: 8\r\n\r\ncurl6-ok";
  send(cfd, resp, strlen(resp), 0);
  close(cfd);
  close(lfd);
  return 0;
}

static int test_curl_ipv6(void) {
  unsigned short port = 3267;
  int pid = fork();
  if (pid < 0) { fail("curl-ipv6"); return -1; }
  if (pid == 0) _exit(run_server6_http(port));

  sleep(1);
  int cpid = fork();
  if (cpid < 0) { fail("curl-ipv6"); return -1; }
  if (cpid == 0) {
    char *argv[] = {"/bin/curl", "-s", "-S", "-6", "-o", "/tmp/curl6.out",
                    "http://[::1]:3267/curl-v6-test", NULL};
    char *envp[] = {NULL};
    execve("/bin/curl", argv, envp);
    _exit(127);
  }
  int cst = 0;
  waitpid(cpid, &cst, 0);

  /* If the client failed to connect, the server is still blocked in accept()
   * with no timeout — force it out so the waitpid below cannot hang the whole
   * suite until the QEMU watchdog. Harmless if it already exited. SIGKILL,
   * because the runner's `trap '' TERM` is inherited by this whole tree. */
  kill(pid, SIGKILL);
  int sst = 0;
  waitpid(pid, &sst, 0);

  if (!WIFEXITED(cst) || WEXITSTATUS(cst) != 0) {
    fail("curl-ipv6");
    return -1;
  }

  int fd = open("/tmp/curl6.out", O_RDONLY);
  if (fd < 0) {
    fail("curl-ipv6");
    return -1;
  }
  char buf[32];
  int r = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  unlink("/tmp/curl6.out");
  if (r != 8 || memcmp(buf, "curl6-ok", 8) != 0) {
    fail("curl-ipv6");
    return -1;
  }
  ok("curl-ipv6");
  return 0;
}






/* The control for the keepalive check: the same idle wait on a connection with
 * keepalive OFF. If this one also loses the connection, the probes are
 * innocent and something else drops idle connections — which is a much bigger
 * bug, and the two markers tell them apart. */
static int test_idle_connection(void) {
  unsigned short port = 3402;
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0 || fd < 0) { fail("idle-connection"); return -1; }
  int reuse = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in a;
  make_loopback_addr(port, &a);
  if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 1) < 0) {
    fail("idle-connection"); close(fd); close(lfd); return -1;
  }
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    fail("idle-connection"); close(fd); close(lfd); return -1;
  }
  int sfd = accept(lfd, 0, 0);
  if (sfd < 0) { fail("idle-connection"); close(fd); close(lfd); return -1; }

  sleep(5);

  const char *msg = "idle-still-here";
  if (send(fd, msg, strlen(msg), 0) < 0) {
    fail("idle-connection");
    close(sfd); close(fd); close(lfd); return -1;
  }
  char buf[64];
  int n = recv_with_retry(sfd, buf, sizeof(buf) - 1);
  if (n <= 0) {
    fail("idle-connection");
    close(sfd); close(fd); close(lfd); return -1;
  }
  buf[n] = '\0';
  if (strcmp(buf, msg) != 0) {
    fail("idle-connection");
    close(sfd); close(fd); close(lfd); return -1;
  }
  ok("idle-connection");
  close(sfd); close(fd); close(lfd);
  return 0;
}

/* Keepalive: the options, and the probes they turn on.
 *
 * Setting a value is the easy half. The half that matters is that the probe
 * b1nix sends is a segment the peer accepts: a keepalive carries a sequence
 * number one behind the next byte, and getting that wrong earns an RST that
 * kills the connection instead of confirming it. So the check sets a one
 * second idle time with a one second interval, leaves an established loopback
 * connection alone for long enough for several probes to go out and be
 * answered, and then requires the connection to still carry data. */
static int test_tcp_keepalive(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { fail("keepalive-create"); return -1; }

  int v = 0;
  socklen_t vlen = sizeof(v);
  if (getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, &vlen) != 0 || v != 7200) {
    fail("keepalive-defaults"); close(fd); return -1;
  }
  v = 0; vlen = sizeof(v);
  if (getsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &v, &vlen) != 0 || v != 9) {
    fail("keepalive-defaults"); close(fd); return -1;
  }
  ok("keepalive-defaults");

  int zero = 0;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &zero, sizeof(zero)) == 0) {
    fail("keepalive-rejects-zero"); close(fd); return -1;
  }
  ok("keepalive-rejects-zero");

  int one = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &one, sizeof(one)) != 0 ||
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &one, sizeof(one)) != 0) {
    fail("keepalive-set"); close(fd); return -1;
  }
  int cnt = 8;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) != 0) {
    fail("keepalive-set"); close(fd); return -1;
  }
  v = 0; vlen = sizeof(v);
  if (getsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, &vlen) != 0 || v != 1) {
    fail("keepalive-set"); close(fd); return -1;
  }
  ok("keepalive-set");
  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) != 0) {
    fail("keepalive-set"); close(fd); return -1;
  }

  unsigned short port = 3401;
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) { fail("keepalive-live"); close(fd); return -1; }
  int reuse = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in a;
  make_loopback_addr(port, &a);
  if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 1) < 0) {
    fail("keepalive-live"); close(fd); close(lfd); return -1;
  }
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    fail("keepalive-live"); close(fd); close(lfd); return -1;
  }
  int sfd = accept(lfd, 0, 0);
  if (sfd < 0) { fail("keepalive-live"); close(fd); close(lfd); return -1; }

  /* Idle for long enough that several probes are due. */
  sleep(5);

  const char *msg = "keepalive-still-here";
  if (send(fd, msg, strlen(msg), 0) < 0) {
    fail("keepalive-live"); close(sfd); close(fd); close(lfd); return -1;
  }
  char buf[64];
  int n = recv_with_retry(sfd, buf, sizeof(buf) - 1);
  if (n <= 0) { fail("keepalive-live"); close(sfd); close(fd); close(lfd); return -1; }
  buf[n] = '\0';
  if (strcmp(buf, msg) != 0) {
    fail("keepalive-live"); close(sfd); close(fd); close(lfd); return -1;
  }
  ok("keepalive-live");

  close(sfd); close(fd); close(lfd);

  /* A datagram socket has no keepalive to configure. */
  int ufd = socket(AF_INET, SOCK_DGRAM, 0);
  if (ufd >= 0) {
    if (setsockopt(ufd, IPPROTO_TCP, TCP_KEEPIDLE, &one, sizeof(one)) == 0) {
      fail("keepalive-udp-refused"); close(ufd); return -1;
    }
    ok("keepalive-udp-refused");
    close(ufd);
  }
  return 0;
}

/* M32b: socket option / address / shutdown API hardening. */
static int test_socket_options(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { fail("sockopt-create"); return -1; }

  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
    fail("sockopt-reuseaddr"); close(fd); return -1;
  }
  int val = 0; socklen_t vlen = sizeof(val);
  if (getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, &vlen) != 0 || val != 1) {
    fail("sockopt-reuseaddr"); close(fd); return -1;
  }
  ok("sockopt-reuseaddr");

  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
    fail("sockopt-nodelay"); close(fd); return -1;
  }
  val = 0; vlen = sizeof(val);
  if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, &vlen) != 0 || val != 1) {
    fail("sockopt-nodelay"); close(fd); return -1;
  }
  ok("sockopt-nodelay");

  val = 0; vlen = sizeof(val);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &val, &vlen) != 0 ||
      val != SOCK_STREAM) {
    fail("sockopt-sotype"); close(fd); return -1;
  }
  ok("sockopt-sotype");

  /* getsockname reports the address we bound to. */
  unsigned short port = 3399;
  struct sockaddr_in baddr;
  make_loopback_addr(port, &baddr);
  if (bind(fd, (struct sockaddr *)&baddr, sizeof(baddr)) != 0) {
    fail("getsockname-bind"); close(fd); return -1;
  }
  struct sockaddr_in got;
  socklen_t glen = sizeof(got);
  memset(&got, 0, sizeof(got));
  if (getsockname(fd, (struct sockaddr *)&got, &glen) != 0 ||
      got.sin_family != AF_INET || got.sin_port != htons(port)) {
    fail("getsockname"); close(fd); return -1;
  }
  ok("getsockname");
  close(fd);

  /* getpeername + shutdown half-close semantics need a connected pair. The
   * child accepts once, reads the single "hello", then exits — it does NOT wait
   * for EOF, because shutdown() here is a local half-close (correct POSIX
   * caller-side semantics); peer FIN-on-shutdown is delivered by close(). */
  unsigned short sport = 3400;
  int pid = fork();
  if (pid < 0) { fail("sockopt-fork"); return -1; }
  if (pid == 0) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in la;
    make_loopback_addr(sport, &la);
    if (bind(lfd, (struct sockaddr *)&la, sizeof(la)) < 0 ||
        listen(lfd, 1) < 0) { _exit(3); }
    int cfd = accept(lfd, 0, 0);
    if (cfd < 0) { _exit(4); }
    char b[64];
    int r = recv_with_retry(cfd, b, sizeof(b));
    close(cfd); close(lfd);
    _exit(r == 5 ? 0 : 5);
  }

  int cfd = connect_loopback(sport);
  if (cfd < 0) { fail("getpeername-connect"); return -1; }
  struct sockaddr_in peer;
  socklen_t plen = sizeof(peer);
  memset(&peer, 0, sizeof(peer));
  if (getpeername(cfd, (struct sockaddr *)&peer, &plen) != 0 ||
      peer.sin_family != AF_INET || peer.sin_port != htons(sport) ||
      peer.sin_addr.s_addr != inet_addr("127.0.0.1")) {
    fail("getpeername"); close(cfd); return -1;
  }
  ok("getpeername");

  send(cfd, "hello", 5, 0);
  /* SHUT_WR closes the write half: further send() must fail with EPIPE. */
  if (shutdown(cfd, SHUT_WR) != 0) { fail("shutdown-wr"); close(cfd); return -1; }
  if (send(cfd, "x", 1, 0) >= 0 || errno != EPIPE) {
    fail("shutdown-wr"); close(cfd); return -1;
  }
  /* SHUT_RD closes the read half: recv() must report EOF (0) immediately. */
  if (shutdown(cfd, SHUT_RD) != 0) { fail("shutdown-wr"); close(cfd); return -1; }
  char rb[8];
  if (recv(cfd, rb, sizeof(rb), 0) != 0) {
    fail("shutdown-wr"); close(cfd); return -1;
  }
  int st = 0;
  waitpid(pid, &st, 0);
  close(cfd);
  ok("shutdown-wr");
  return 0;
}

/* M32b: pseudo-terminal substrate (/dev/ptmx, pts, line discipline). */
static int test_pty(void) {
  /* The hangup test closes the master, which delivers SIGHUP to the slave's
   * foreground group — here, ours. Ignore it so the test process survives. */
  signal(SIGHUP, SIG_IGN);

  int m, s;
  char name[64];
  if (openpty(&m, &s, name, NULL, NULL) != 0) {
    emit("M32B-PTY: FAIL openpty\n");
    return -1;
  }
  char *pn = ptsname(m);
  if (!pn || strncmp(pn, "/dev/pts/", 9) != 0 || strcmp(pn, name) != 0) {
    emit("M32B-PTY: FAIL ptsname\n");
    close(m); close(s); return -1;
  }
  emit("M32B-PTY: ok openpty\n");

  /* window size round-trips through the slave. */
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = 30; ws.ws_col = 100;
  struct winsize got;
  memset(&got, 0, sizeof(got));
  if (ioctl(s, TIOCSWINSZ, &ws) != 0 || ioctl(s, TIOCGWINSZ, &got) != 0 ||
      got.ws_row != 30 || got.ws_col != 100) {
    emit("M32B-PTY: FAIL winsize\n");
    close(m); close(s); return -1;
  }
  emit("M32B-PTY: ok winsize\n");

  /* Canonical mode: a line written to the master is delivered to the slave on
   * the newline, and echoed back on the master (ONLCR -> "hi\r\n"). */
  write(m, "hi\n", 3);
  char lb[32];
  int n = (int)read(s, lb, sizeof(lb));
  if (n != 3 || memcmp(lb, "hi\n", 3) != 0) {
    emit("M32B-PTY: FAIL canonical\n");
    close(m); close(s); return -1;
  }
  emit("M32B-PTY: ok canonical\n");
  char eb[32];
  int en = (int)read(m, eb, sizeof(eb));
  if (en < 2 || eb[0] != 'h' || eb[1] != 'i') {
    emit("M32B-PTY: FAIL echo\n");
    close(m); close(s); return -1;
  }
  emit("M32B-PTY: ok echo\n");

  /* Raw mode: no echo, single-byte immediate delivery. */
  struct termios raw;
  tcgetattr(s, &raw);
  cfmakeraw(&raw);
  tcsetattr(s, TCSANOW, &raw);
  write(m, "Z", 1);
  char rb[4];
  int rn = (int)read(s, rb, sizeof(rb));
  if (rn != 1 || rb[0] != 'Z') {
    emit("M32B-PTY: FAIL raw\n");
    close(m); close(s); return -1;
  }
  emit("M32B-PTY: ok raw\n");

  /* Hangup: closing the master makes the slave read report EOF. */
  close(m);
  char hb[4];
  if (read(s, hb, sizeof(hb)) != 0) {
    emit("M32B-PTY: FAIL hangup\n");
    close(s); return -1;
  }
  emit("M32B-PTY: ok hangup\n");
  close(s);
  emit("M32B-PTY: done\n");
  return 0;
}

/* M32b: login/session plumbing — env survives execve (crt0 -> environ -> getenv),
 * the mechanism /bin/login uses to hand a real environment to the login shell. */
static int test_session(void) {
  int pid = fork();
  if (pid < 0) { emit("M32B-SESS: FAIL fork\n"); return -1; }
  if (pid == 0) {
    char *av[] = {"/bin/m32-smoke", "--envcheck", 0};
    char *ev[] = {"B1NIX_SESS=loginok", 0};
    execve("/bin/m32-smoke", av, ev);
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 42) {
    emit("M32B-SESS: FAIL env-execve\n");
    return -1;
  }
  emit("M32B-SESS: ok env-execve\n");
  return 0;
}

/* M32b: crypto/RNG baseline. getrandom (secure random bytes), a standalone
 * SHA-512 (FIPS 180-4 test vector below) plus musl crypt(3)'s "$6$" SHA-512
 * password KDF (what dropbear's password auth verifies against /etc/shadow),
 * and a constant-time-style sensitivity check. The full bundled libtomcrypt
 * SSH primitives (curve25519/ed25519/chacha20poly1305/aes) build for b1nix
 * and are exercised end-to-end by the real SSH handshake.
 *
 * sha512() below is a small self-contained FIPS 180-4 implementation (not
 * linked from a shared crypto library) — this test only needs one one-shot
 * digest to sanity-check the test vector, and every crypto library already
 * in this build (mbedTLS, libtomcrypt) is wired into other, unrelated
 * targets rather than this one, so pulling one in here for a single call
 * would be a new build-graph dependency for no real gain. */
static const uint64_t sha512_k[80] = {
  0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
  0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
  0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
  0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
  0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
  0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
  0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
  0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
  0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
  0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
  0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
  0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
  0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
  0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
  0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
  0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static uint64_t sha512_ror(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512(const void *data, size_t len, unsigned char out[64]) {
  uint64_t h[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
  };
  size_t bitlen_off = ((len + 17 + 127) / 128) * 128;
  unsigned char *msg = calloc(1, bitlen_off);
  memcpy(msg, data, len);
  msg[len] = 0x80;
  uint64_t bitlen = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++)
    msg[bitlen_off - 1 - i] = (unsigned char)(bitlen >> (8 * i));

  for (size_t off = 0; off < bitlen_off; off += 128) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
      w[i] = 0;
      for (int b = 0; b < 8; b++)
        w[i] = (w[i] << 8) | msg[off + i * 8 + b];
    }
    for (int i = 16; i < 80; i++) {
      uint64_t s0 = sha512_ror(w[i-15], 1) ^ sha512_ror(w[i-15], 8) ^ (w[i-15] >> 7);
      uint64_t s1 = sha512_ror(w[i-2], 19) ^ sha512_ror(w[i-2], 61) ^ (w[i-2] >> 6);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint64_t a = h[0], b_ = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
      uint64_t S1 = sha512_ror(e, 14) ^ sha512_ror(e, 18) ^ sha512_ror(e, 41);
      uint64_t ch = (e & f) ^ (~e & g);
      uint64_t t1 = hh + S1 + ch + sha512_k[i] + w[i];
      uint64_t S0 = sha512_ror(a, 28) ^ sha512_ror(a, 34) ^ sha512_ror(a, 39);
      uint64_t maj = (a & b_) ^ (a & c) ^ (b_ & c);
      uint64_t t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1;
      d = c; c = b_; b_ = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b_; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  free(msg);
  for (int i = 0; i < 8; i++)
    for (int b = 0; b < 8; b++)
      out[i * 8 + b] = (unsigned char)(h[i] >> (56 - 8 * b));
}

static int test_crypto(void) {
  unsigned char r1[32], r2[32];
  if (getrandom(r1, sizeof(r1), 0) != (ssize_t)sizeof(r1)) {
    emit("M32B-CRYPTO: FAIL getrandom\n");
    return -1;
  }
  getrandom(r2, sizeof(r2), 0);
  int allzero = 1, same = 1;
  for (int i = 0; i < 32; i++) {
    if (r1[i]) allzero = 0;
    if (r1[i] != r2[i]) same = 0;
  }
  if (allzero || same) {
    emit("M32B-CRYPTO: FAIL getrandom\n");
    return -1;
  }
  emit("M32B-CRYPTO: ok getrandom\n");

  /* SHA-512("abc") = ddaf35a193617aba...a54ca49f (FIPS 180-4 vector). */
  unsigned char d[64];
  sha512("abc", 3, d);
  if (d[0] != 0xdd || d[1] != 0xaf || d[2] != 0x35 || d[63] != 0x9f) {
    emit("M32B-CRYPTO: FAIL sha512\n");
    return -1;
  }
  emit("M32B-CRYPTO: ok sha512\n");

  /* crypt() must be deterministic for a given password+salt and sensitive to a
   * wrong password (the property /etc/shadow verification relies on). The old
   * in-house "$b1$" scheme went away with the musl migration (see
   * m31_smoke.c's test_shadow_readable) — /etc/shadow, dropbear, login, and su
   * all use musl crypt(3)'s standard SHA-512 "$6$" format now, so exercise
   * that instead. */
  char *p = crypt("hunter2", "$6$testsalt$");
  if (!p) { emit("M32B-CRYPTO: FAIL crypt\n"); return -1; }
  char hcopy[160];
  strncpy(hcopy, p, sizeof(hcopy) - 1);
  hcopy[sizeof(hcopy) - 1] = '\0';
  p = crypt("hunter2", "$6$testsalt$");
  if (!p || strcmp(hcopy, p) != 0) { emit("M32B-CRYPTO: FAIL crypt\n"); return -1; }
  p = crypt("wrongpw", "$6$testsalt$");
  if (!p || strcmp(hcopy, p) == 0) { emit("M32B-CRYPTO: FAIL crypt\n"); return -1; }
  emit("M32B-CRYPTO: ok crypt\n");
  emit("M32B-CRYPTO: done\n");
  return 0;
}

/* Forward declaration — defined below as an SSH helper. */
static int ssh_ensure_hostkey(void);

/* M32b: prove the Dropbear binary executes on b1nix by verifying the
 * Ed25519 host key.  We actively ensure the key exists (creating /etc/ssh
 * and running dropbearkey if needed) so the test is self-sufficient and
 * does not depend on the boot-time init script having succeeded. */
static int test_dropbear_keygen(void) {
  const char *path = "/etc/ssh/hk_ed25519";

  /* Actively ensure the directory and key exist. */
  if (ssh_ensure_hostkey() != 0) {
    emit("M32B-SSH: FAIL ensure-hostkey\n");
    return -1;
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    emit("M32B-SSH: FAIL keyfile-missing\n");
    return -1;
  }
  char buf[64];
  int n = (int)read(fd, buf, sizeof(buf));
  close(fd);
  if (n < 32) {
    emit("M32B-SSH: FAIL keyfile-size\n");
    return -1;
  }
  emit("M32B-SSH: ok dropbearkey\n");
  return 0;
}

/* ── M32b SSH helpers (shared by the positive, negative-auth, and PTY tests) ──
 *
 * One dropbear server (fork-per-connection) serves every sub-test, so the
 * accept→fork→serve path is exercised repeatedly on a single instance. */

/* Generate the persistent Ed25519 host key once if it is not already present. */
static int ssh_ensure_hostkey(void) {
  mkdir("/etc/ssh", 0700);

  int fd = open("/etc/ssh/hk_ed25519", O_RDONLY);
  if (fd >= 0) {
    close(fd);
    return 0;
  }

  int kp = fork();
  if (kp < 0) {
    return -1;
  }
  if (kp == 0) {
    char *av[] = {"/bin/dropbearkey", "-t", "ed25519", "-f",
                  "/etc/ssh/hk_ed25519", 0};
    char *ev[] = {"HOME=/root", "PATH=/bin", 0};
    execve("/bin/dropbearkey", av, ev);
    _exit(127);
  }
  int st = 0;
  waitpid(kp, &st, 0);
  return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* Fork a foreground dropbear bound to a loopback port; returns the server pid. */
static int ssh_start_server(const char *portspec) {
  int srv = fork();
  if (srv == 0) {
    char *av[] = {"/bin/dropbear", "-r", "/etc/ssh/hk_ed25519",
                  "-p", (char *)portspec, "-F", 0};
    char *ev[] = {0};
    execve("/bin/dropbear", av, ev);
    _exit(127);
  }
  sleep(3); /* let the server load the key and bind */
  return srv;
}

/* Reap a client without ever blocking the suite. The loopback handshake is
 * paced by net_task (one tick per round trip), so poll WNOHANG with a real
 * sleep between checks, give up after ~20s, and SIGKILL a stuck client.
 * Returns 1 if the client exited on its own, 0 if it had to be killed. */
static int ssh_reap_client(int cli, int *status) {
  for (int i = 0; i < 20; i++) {
    int r = (int)waitpid(cli, status, WNOHANG);
    if (r == cli) return 1;
    if (r < 0) return 0;
    /* Progress trace: the whole suite has been killed here by the init
     * watchdog before (sleep never returning), and without a per-poll line the
     * log cannot tell "client still running" from "parent stuck in sleep". */
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "M32B-SSH: dbg reap poll=%d\n", i);
    emit(dbg);
    sleep(1);
  }
  kill(cli, SIGKILL);
  return 0;
}

/* Bounded teardown: SIGTERM+SIGKILL then WNOHANG-reap so a listener parked in a
 * blocking accept() can never hang the suite (worst case leaks one task). */
static void ssh_kill_server(int srv) {
  kill(srv, SIGTERM);
  kill(srv, SIGKILL);
  for (int i = 0; i < 200; i++) {
    int r = (int)waitpid(srv, NULL, WNOHANG);
    if (r == srv || r < 0)
      break;
    sched_yield();
  }
}

/* Positive path: full KEX + chacha20-poly1305 + password auth (verified against
 * /etc/shadow) + a single remote command, captured to a file. */
static int ssh_test_login(void) {
  unlink("/tmp/ssh_out");
  unlink("/tmp/ssh_ran");
  int cli = fork();
  if (cli == 0) {
    int o = open("/tmp/ssh_out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (o >= 0) { dup2(o, 1); if (o > 2) close(o); }
    int e = open("/tmp/ssh_err", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (e >= 0) { dup2(e, 2); if (e > 2) close(e); }
    int z = open("/dev/null", O_RDONLY);
    if (z >= 0) { dup2(z, 0); if (z > 2) close(z); }
    char *av[] = {"/bin/dbclient", "-y", "-y", "-p", "2222",
                  "root@127.0.0.1",
                  "echo RAN > /tmp/ssh_ran; echo M32B-SSH-LOGIN-OK", 0};
    char *ev[] = {"DROPBEAR_PASSWORD=root", 0};
    execve("/bin/dbclient", av, ev);
    _exit(127);
  }
  /* Wait on the OUTPUT, not on the client's exit. The pass condition is the
   * marker in the file — that alone proves KEX + auth + remote exec — and the
   * client may well outlive it (loopback connection-close pacing). Poll for up
   * to 90 s: the remote login shell here is zsh, and its FIRST exec in a lane
   * is a cold one (this arch loads ELF segments eagerly, so the whole binary
   * and its libraries come off the disk before main runs). The old 20 s bound
   * cut exactly that cold start off; every later zsh in the same lane — the
   * pty check right after this one — starts from cache and finishes at once. */
  int cst = 0, done = 0;
  char buf[256];
  int n = -1;
  for (int i = 0; i < 90; i++) {
    int fd = open("/tmp/ssh_out", O_RDONLY);
    n = fd >= 0 ? (int)read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    buf[n > 0 ? n : 0] = '\0';
    if (strstr(buf, "M32B-SSH-LOGIN-OK"))
      break;
    if (waitpid(cli, &cst, WNOHANG) == cli) {
      done = 1;
      break;
    }
    /* Progress trace: the harness kills an instance that goes quiet, and this
     * wait is the longest silent stretch in the suite. */
    if (i % 5 == 0) {
      char dbg[64];
      snprintf(dbg, sizeof(dbg), "M32B-SSH: dbg login wait=%d\n", i);
      emit(dbg);
    }
    sleep(1);
  }
  if (!done && waitpid(cli, &cst, WNOHANG) != cli)
    kill(cli, SIGKILL);
  {
    int fd = open("/tmp/ssh_out", O_RDONLY);
    n = fd >= 0 ? (int)read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    buf[n > 0 ? n : 0] = '\0';
  }
  if (!strstr(buf, "M32B-SSH-LOGIN-OK")) {
    char ran[64] = {0}, err[192] = {0};
    int rf = open("/tmp/ssh_ran", O_RDONLY);
    if (rf >= 0) { int rn = (int)read(rf, ran, sizeof(ran) - 1); ran[rn > 0 ? rn : 0] = 0; close(rf); }
    int ef = open("/tmp/ssh_err", O_RDONLY);
    if (ef >= 0) { int en = (int)read(ef, err, sizeof(err) - 1); err[en > 0 ? en : 0] = 0; close(ef); }
    for (int i = 0; err[i]; i++) if (err[i] == '\n') err[i] = '|';
    for (int i = 0; ran[i]; i++) if (ran[i] == '\n') ran[i] = '|';
    char dbg[640];
    snprintf(dbg, sizeof(dbg),
             "M32B-SSH: dbg handshake done=%d cli=0x%x out=[%s] ran=[%s] err=[%s]\n",
             done, cst, buf, ran, err);
    emit(dbg);
    emit("M32B-SSH: FAIL handshake\n");
    return -1;
  }
  emit("M32B-SSH: ok handshake\n");
  return 0;
}

/* Negative auth: a WRONG password must be rejected by the daemon — the remote
 * command must never run, and the client must terminate rather than hang. The
 * client cannot fall back to an interactive prompt here: DROPBEAR_PASSWORD is
 * reused on every attempt, so the server disconnects after its auth-try limit
 * (and stdin is /dev/null as a belt-and-suspenders against any tty fallback). */
static int ssh_test_negauth(void) {
  unlink("/tmp/ssh_neg");
  int cli = fork();
  if (cli == 0) {
    int z = open("/dev/null", O_RDONLY);
    if (z >= 0) { dup2(z, 0); if (z > 2) close(z); }
    int o = open("/tmp/ssh_neg", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (o >= 0) { dup2(o, 1); dup2(o, 2); if (o > 2) close(o); }
    char *av[] = {"/bin/dbclient", "-y", "-y", "-p", "2222",
                  "root@127.0.0.1", "echo", "SHOULD-NOT-RUN", 0};
    char *ev[] = {"DROPBEAR_PASSWORD=wrongpw", 0};
    execve("/bin/dbclient", av, ev);
    _exit(127);
  }
  int cst = 0;
  int done = ssh_reap_client(cli, &cst);
  char buf[256];
  int fd = open("/tmp/ssh_neg", O_RDONLY);
  int n = fd >= 0 ? (int)read(fd, buf, sizeof(buf) - 1) : -1;
  if (fd >= 0) close(fd);
  buf[n > 0 ? n : 0] = '\0';
  if (!done) {
    emit("M32B-SSH: FAIL negauth-hung\n");
    return -1;
  }
  if (strstr(buf, "SHOULD-NOT-RUN")) {
    /* the daemon accepted a wrong password and ran the command — a real bug */
    emit("M32B-SSH: FAIL negauth-accepted\n");
    return -1;
  }
  emit("M32B-SSH: ok negauth\n");
  return 0;
}

/* Command over a remote PTY. `dbclient -t <cmd>` forces a remote pseudo-terminal
 * AND runs a command on it (like `ssh -t host cmd`): the daemon allocates a pty,
 * spawns the login shell (/bin/sh) on the slave, and runs the command there.
 * This exercises the sshd pty path + login shell + command exec. dbclient also
 * requires its OWN stdin to be a tty (it puts the local terminal in raw mode),
 * so we give the client a local pty slave via login_tty. The command writes a
 * marker FILE — checked over the shared VFS, not the pty output, so the remote
 * pty ECHO of the command line can't cause a false pass. */
static int ssh_test_pty(void) {
  unlink("/tmp/ptyresult");
  int master, slave;
  char ptyname[64];
  if (openpty(&master, &slave, ptyname, NULL, NULL) != 0) {
    emit("M32B-SSH: FAIL pty-openpty\n");
    return -1;
  }
  int cli = fork();
  if (cli == 0) {
    close(master);
    login_tty(slave); /* setsid + controlling tty + slave -> stdin/out/err */
    char *av[] = {"/bin/dbclient", "-y", "-y", "-t", "-p", "2222",
                  "root@127.0.0.1",
                  "echo M32B-SSH-PTY-OK > /tmp/ptyresult", 0};
    char *ev[] = {"DROPBEAR_PASSWORD=root", "TERM=vt100", 0};
    execve("/bin/dbclient", av, ev);
    _exit(127);
  }
  close(slave);
  int cst = 0;
  int done = ssh_reap_client(cli, &cst);
  /* Drain anything the remote pty echoed back so a full master buffer can't
   * wedge the remote side mid-teardown (and so we can surface it on failure).
   * This MUST be bounded: the master fd is blocking, and under -smp the remote
   * slave may not have been released yet when we get here (dbclient/its shell
   * still tearing down on another CPU), so a plain read(master) would park in
   * the kernel's pty_master_read forever and hang the whole suite — this was
   * the silent ~33% -smp "fork/exec" flake. select() with a finite per-attempt
   * timeout caps the wait (~2 s total) and stops on EOF (slave closed). */
  char drain[256];
  int dn = 0;
  for (int tries = 0; tries < 20 && dn < (int)sizeof(drain) - 1; tries++) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(master, &rfds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000}; /* 100 ms */
    int sr = select(master + 1, &rfds, 0, 0, &tv);
    if (sr <= 0)
      break; /* timeout or error: stop draining, never block */
    int r = (int)read(master, drain + dn, (size_t)(sizeof(drain) - 1 - dn));
    if (r <= 0)
      break; /* EOF (slave gone) or error */
    dn += r;
  }
  drain[dn > 0 ? dn : 0] = '\0';
  for (int i = 0; drain[i]; i++)
    if (drain[i] == '\n' || drain[i] == '\r') drain[i] = '|';
  close(master);

  char buf[128];
  int fd = open("/tmp/ptyresult", O_RDONLY);
  int n = fd >= 0 ? (int)read(fd, buf, sizeof(buf) - 1) : -1;
  if (fd >= 0) close(fd);
  buf[n > 0 ? n : 0] = '\0';
  if (!strstr(buf, "M32B-SSH-PTY-OK")) {
    char dbg[400];
    snprintf(dbg, sizeof(dbg),
             "M32B-SSH: dbg pty done=%d cli=0x%x result=[%s] pty=[%s]\n",
             done, cst, buf, drain);
    emit(dbg);
    emit("M32B-SSH: FAIL pty\n");
    return -1;
  }
  emit("M32B-SSH: ok pty\n");
  return 0;
}

static int test_sshd_service(void) {
  /* 1. Verify init.d script exists and is executable. */
  int fd = open("/etc/init.d/sshd", O_RDONLY);
  if (fd < 0) {
    emit("M32B-SSH: FAIL service-script-missing\n");
    return -1;
  }
  close(fd);

  /* 2. Since init ran it, check if pid file exists. */
  /* Wait a little for it to start up from init. */
  sleep(1);
  int pid_fd = open("/var/run/sshd.pid", O_RDONLY);
  if (pid_fd < 0) {
    emit("M32B-SSH: FAIL service-pid-missing\n");
    return -1;
  }
  char pid_str[64];
  int r = read(pid_fd, pid_str, sizeof(pid_str) - 1);
  close(pid_fd);
  if (r <= 0) {
    emit("M32B-SSH: FAIL service-pid-empty\n");
    return -1;
  }
  pid_str[r] = '\0';
  int daemon_pid = atoi(pid_str);
  if (daemon_pid <= 0) {
    char dbg[128];
    snprintf(dbg, sizeof(dbg), "M32B-SSH: FAIL service-pid-invalid (content: '%s')\n", pid_str);
    emit(dbg);
    return -1;
  }

  /* 3. Check status. */
  int p = fork();
  if (p == 0) {
    char *av[] = {"/bin/sh", "/etc/init.d/sshd", "status", 0};
    char *ev[] = {0};
    execve("/bin/sh", av, ev);
    _exit(127);
  }
  int st = 0;
  waitpid(p, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    emit("M32B-SSH: FAIL service-status\n");
    return -1;
  }

  /* 4. Check log output. */
  int log_fd = open("/var/log/sshd.log", O_RDONLY);
  if (log_fd < 0) {
    emit("M32B-SSH: FAIL service-log-missing\n");
    return -1;
  }
  char log_buf[64];
  int log_read = read(log_fd, log_buf, sizeof(log_buf) - 1);
  close(log_fd);
  if (log_read <= 0) {
    emit("M32B-SSH: FAIL service-log-empty\n");
    return -1;
  }

  /* 5. Test clean shutdown (stop). */
  p = fork();
  if (p == 0) {
    char *av[] = {"/bin/sh", "/etc/init.d/sshd", "stop", 0};
    char *ev[] = {0};
    execve("/bin/sh", av, ev);
    _exit(127);
  }
  st = 0;
  waitpid(p, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    emit("M32B-SSH: FAIL service-stop\n");
    return -1;
  }

  /* Verify PID file is gone and daemon process is terminated. */
  pid_fd = open("/var/run/sshd.pid", O_RDONLY);
  if (pid_fd >= 0) {
    close(pid_fd);
    emit("M32B-SSH: FAIL service-pid-not-removed\n");
    return -1;
  }
  
  /* Verify daemon process is dead (kill with 0 should return ESRCH/EPERM/etc or fail if gone). */
  if (kill(daemon_pid, 0) == 0) {
    emit("M32B-SSH: FAIL service-process-still-running\n");
    return -1;
  }

  /* 6. Test restart/start. */
  p = fork();
  if (p == 0) {
    char *av[] = {"/bin/sh", "/etc/init.d/sshd", "start", 0};
    char *ev[] = {0};
    execve("/bin/sh", av, ev);
    _exit(127);
  }
  st = 0;
  waitpid(p, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    emit("M32B-SSH: FAIL service-start\n");
    return -1;
  }
  
  sleep(1);
  pid_fd = open("/var/run/sshd.pid", O_RDONLY);
  if (pid_fd < 0) {
    emit("M32B-SSH: FAIL service-restart-pid-missing\n");
    return -1;
  }
  close(pid_fd);

  emit("M32B-SSH: ok service-lifecycle\n");
  return 0;
}

/* M32b: end-to-end localhost SSH against one dropbear instance — exercises TCP
 * loopback, the full SSH KEX + chacha20-poly1305, server fork-per-connection,
 * and three login paths: a positive password login running a remote command, a
 * negative wrong-password rejection, and an interactive shell over a remote
 * PTY. Each sub-test emits its own M32B-SSH ok/FAIL marker (non-fatal). */
static int test_ssh_handshake(void) {
  test_sshd_service();
  if (ssh_ensure_hostkey() != 0) {
    emit("M32B-SSH: FAIL handshake-hostkey\n");
    return -1;
  }
  int srv = ssh_start_server("127.0.0.1:2222");
  /* Stage markers: when one sub-test wedges the whole suite is killed by the
   * init watchdog, and the last stage line is the only thing that names which
   * step never returned. */
  emit("M32B-SSH: stage login\n");
  ssh_test_login();
  emit("M32B-SSH: stage negauth\n");
  ssh_test_negauth();
  emit("M32B-SSH: stage pty\n");
  ssh_test_pty();
  emit("M32B-SSH: stage teardown\n");
  ssh_kill_server(srv);
  emit("M32B-SSH: stage done\n");
  return 0;
}

static int test_tcp_window_throttle(void) {
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return 1;

  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(39999);

  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(srv);
    return 1;
  }
  if (listen(srv, 1) < 0) {
    close(srv);
    return 1;
  }

  int cli = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (cli < 0) {
    close(srv);
    return 1;
  }
  connect(cli, (struct sockaddr *)&addr, sizeof(addr));

  struct sockaddr_in peer;
  socklen_t peerlen = sizeof(peer);
  int acc = accept(srv, (struct sockaddr *)&peer, &peerlen);
  if (acc < 0) {
    close(cli);
    close(srv);
    return 1;
  }
  fcntl(acc, F_SETFL, fcntl(acc, F_GETFL, 0) | O_NONBLOCK);

  int small_rcv = 4096;
  setsockopt(acc, SOL_SOCKET, SO_RCVBUF, &small_rcv, sizeof(small_rcv));

  usleep(50000);

  char buf[1024];
  memset(buf, 'A', sizeof(buf));
  int stalled = 0;
  for (int i = 0; i < 500; i++) {
    ssize_t n = send(cli, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0 && errno == EAGAIN) {
      stalled = 1;
      break;
    }
    if (n < 0) break;
  }

  close(acc);
  close(cli);
  close(srv);

  if (stalled)
    emit("M32-TCP: ok window-throttle\n");
  else
    emit("M32-TCP: FAIL window-throttle\n");
  return stalled ? 0 : 1;
}

int main(int argc, char **argv) {
  /* Self-reexec env probe (see test_session): report whether the env we were
   * exec'd with reached getenv(). */
  if (argc >= 2 && strcmp(argv[1], "--envcheck") == 0) {
    const char *v = getenv("B1NIX_SESS");
    _exit(v && strcmp(v, "loginok") == 0 ? 42 : 1);
  }
  emit("M32-NET: start\n");
  if (test_pty() != 0)                 return 1;
  if (test_session() != 0)             return 1;
  if (test_crypto() != 0)              return 1;
  if (test_dropbear_keygen() != 0)     return 1;
  if (test_socket_options() != 0)      return 1;
  if (test_idle_connection() != 0)     return 1;
  if (test_tcp_keepalive() != 0)       return 1;
  test_external_net();
  if (test_getnameinfo() != 0)         return 1;
  if (test_v4mapped_udp() != 0)        return 1;
  if (test_ipv6_v6only() != 0)         return 1;
  if (test_select_timeout_zero() != 0) return 1;
  if (test_select_pipe_ready() != 0)   return 1;
  if (test_select_multi_fd() != 0)     return 1;
  if (test_dns_libc() != 0)            return 1;
  if (test_udp6_loopback() != 0)       return 1;
  if (test_tcp6_loopback() != 0)       return 1;
  if (test_tcp_window_throttle() != 0) return 1;
  if (test_tcp_client_server() != 0)   return 1;
  if (test_curl_ipv6() != 0)           return 1;
  /* Non-fatal (like test_external_net): the end-to-end SSH login exercises the
   * whole daemon and must not mask the rest of the suite if it regresses. It
   * emits its own M32B-SSH ok/FAIL marker. Run last so its long handshake does
   * not delay the other checks. */
  test_ssh_handshake();
  emit("M32-NET: done\n");
  return 0;
}
