/*
 * net_smoke — network smoke tests ported from deleted kernel/user/programs.c.
 * Tests: ping gateway (NET-SMOKE), UDP send/recv (UDP-SMOKE),
 * poll readiness (POLL-SMOKE), DNS parsing (DNS-SMOKE).
 * Uses standard POSIX socket API instead of kernel-internal vfs_* calls.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void marker(const char *t) { write(1, t, strlen(t)); }

static void test_ping_gateway(void) {
  pid_t pid = fork();
  if (pid == 0) {
    execl("/bin/ping", "ping", "-c", "2", "10.0.2.2", NULL);
    _exit(127);
  }
  if (pid < 0) { marker("NET-SMOKE: fail ping-gateway\n"); return; }
  int st = 0;
  waitpid(pid, &st, 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
    marker("NET-SMOKE: ok ping-gateway\n");
  else
    marker("NET-SMOKE: fail ping-gateway\n");
}

/* Reads the raw ICMP socket until a destination-unreachable / port-unreachable
 * (type 3, code 3) arrives or the budget runs out. SOCK_RAW readers get the IP
 * header included, so the ICMP message starts at the IHL boundary. */
static int wait_port_unreachable(int raw) {
  for (int i = 0; i < 200; i++) {
    unsigned char pkt[512];
    ssize_t n = recv(raw, pkt, sizeof(pkt), MSG_DONTWAIT);
    if (n < 0) {
      usleep(10000);
      continue;
    }
    size_t ihl = (size_t)(pkt[0] & 0x0F) * 4;
    if (ihl < 20 || (size_t)n < ihl + 2)
      continue;
    if (pkt[ihl] == 3 && pkt[ihl + 1] == 3)
      return 1;
  }
  return 0;
}

static void test_udp_send_recv(void) {
  /* Send a probe to an unbound port → expect ICMP port unreachable, observed
   * on a raw ICMP socket (the error comes back over the loopback datapath). */
  int raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { marker("UDP-SMOKE: fail probe-open\n"); if (raw >= 0) close(raw); return; }
  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = htons(54321);
  inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
  sendto(fd, "probe", 5, 0, (struct sockaddr *)&dst, sizeof(dst));
  marker("UDP-SMOKE: probe-sent\n");
  if (raw < 0)
    marker("UDP-SMOKE: fail icmp-raw-open\n");
  else if (wait_port_unreachable(raw))
    marker("UDP-SMOKE: icmp-port-unreachable\n");
  else
    marker("UDP-SMOKE: fail icmp-port-unreachable\n");
  if (raw >= 0)
    close(raw);
  close(fd);

  /* Queue ordering: bind, send two packets to self, verify read order. */
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) { marker("UDP-SMOKE: fail queue-open\n"); return; }
  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_port = htons(55001);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
    marker("UDP-SMOKE: fail queue-bind\n"); close(s); return;
  }
  /* Send to self. */
  sendto(s, "first", 5, 0, (struct sockaddr *)&local, sizeof(local));
  sendto(s, "second", 6, 0, (struct sockaddr *)&local, sizeof(local));
  char out1[16] = {0}, out2[16] = {0};
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
  ssize_t r1 = recv(s, out1, sizeof(out1), 0);
  ssize_t r2 = recv(s, out2, sizeof(out2), 0);
  close(s);
  if (r1 == 5 && r2 == 6 && memcmp(out1, "first", 5) == 0 &&
      memcmp(out2, "second", 6) == 0)
    marker("UDP-SMOKE: queue-2pkt-ok\n");
  else
    marker("UDP-SMOKE: fail queue-order\n");
}

/* TCP-SMOKE: the full stream path — listen, connect, accept, send, recv, and
 * an orderly close observed as EOF by the peer. The pre-netd version of this
 * check injected hand-built segments into the in-kernel stack; with the stack
 * in ring 3 the honest equivalent is a real loopback connection through the
 * socket API. */
static void test_tcp_path(void) {
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) { marker("TCP-SMOKE: fail socket\n"); return; }
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(56001);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(srv, 1) < 0) {
    marker("TCP-SMOKE: fail listen\n");
    close(srv);
    return;
  }

  pid_t pid = fork();
  if (pid < 0) { marker("TCP-SMOKE: fail fork\n"); close(srv); return; }
  if (pid == 0) {
    close(srv);
    int cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli < 0)
      _exit(1);
    if (connect(cli, (struct sockaddr *)&addr, sizeof(addr)) < 0)
      _exit(2);
    if (send(cli, "b1nix-tcp", 9, 0) != 9)
      _exit(3);
    char echoed[16] = {0};
    ssize_t n = recv(cli, echoed, sizeof(echoed), 0);
    close(cli);
    _exit(n == 2 && memcmp(echoed, "ok", 2) == 0 ? 0 : 4);
  }

  int cfd = accept(srv, NULL, NULL);
  if (cfd < 0) {
    marker("TCP-SMOKE: unsupported\n");
    close(srv);
    waitpid(pid, NULL, 0);
    return;
  }
  char buf[16] = {0};
  ssize_t got = recv(cfd, buf, sizeof(buf), 0);
  int sent_ok = (send(cfd, "ok", 2, 0) == 2);
  /* Orderly close: the peer's final recv must see the two bytes, then EOF. */
  close(cfd);
  close(srv);

  int st = 0;
  waitpid(pid, &st, 0);
  if (got == 9 && memcmp(buf, "b1nix-tcp", 9) == 0 && sent_ok &&
      WIFEXITED(st) && WEXITSTATUS(st) == 0)
    marker("TCP-SMOKE: path-exercised\n");
  else
    marker("TCP-SMOKE: fail data\n");
}

static void test_poll_readiness(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { marker("POLL-SMOKE: fail open\n"); return; }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(55002);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    marker("POLL-SMOKE: fail bind\n"); close(fd); return;
  }
  /* A fresh UDP socket should be writable. */
  struct pollfd pfd = {fd, POLLOUT, 0};
  if (poll(&pfd, 1, 0) < 0 || (pfd.revents & POLLOUT) == 0) {
    marker("POLL-SMOKE: fail writable\n"); close(fd); return;
  }
  /* Send a packet to self → should become readable. */
  sendto(fd, "poll", 4, 0, (struct sockaddr *)&addr, sizeof(addr));
  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  if (poll(&pfd, 1, 100) < 0 || (pfd.revents & POLLIN) == 0) {
    marker("POLL-SMOKE: fail readable\n"); close(fd); return;
  }
  close(fd);
  marker("POLL-SMOKE: ready-udp\n");
}

static void test_dns_parse(void) {
  /* A-record: getaddrinfo on a numeric address should succeed. */
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_INET;
  if (getaddrinfo("127.0.0.1", NULL, &hints, &res) == 0 && res) {
    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
      marker("DNS-SMOKE: ok parse-a-record\n");
    else
      marker("DNS-SMOKE: fail parse-a-record\n");
    freeaddrinfo(res);
  } else {
    marker("DNS-SMOKE: fail parse-a-record\n");
  }

  /* AAAA: getaddrinfo for ::1 */
  hints.ai_family = AF_INET6;
  if (getaddrinfo("::1", NULL, &hints, &res) == 0 && res) {
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)res->ai_addr;
    if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr))
      marker("DNS-SMOKE: ok parse-aaaa-record\n");
    else
      marker("DNS-SMOKE: fail parse-aaaa-record\n");
    freeaddrinfo(res);
  } else {
    marker("DNS-SMOKE: fail parse-aaaa-record\n");
  }

  /* A real name, resolved through musl's resolver end to end: query out over
   * UDP, answer back, source address matched, address returned. Three kernel
   * bugs used to make this impossible while every packet arrived correctly —
   * a datagram socket sent with source port 0, recvfrom reported the last
   * send target instead of the sender, and recvmsg zero-filled msg_name
   * (which is the one musl actually uses). Needs the SLIRP DNS at 10.0.2.3,
   * the same external dependency the BusyBox nslookup check already has. */
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo("dl-cdn.alpinelinux.org", "80", &hints, &res) == 0 && res) {
    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    if (sin->sin_addr.s_addr != 0 && sin->sin_port == htons(80))
      marker("DNS-SMOKE: ok resolve-name\n");
    else
      marker("DNS-SMOKE: fail resolve-name\n");
    freeaddrinfo(res);
  } else {
    marker("DNS-SMOKE: fail resolve-name\n");
  }

  /* /etc/resolv.conf check */
  FILE *f = fopen("/etc/resolv.conf", "r");
  if (f) {
    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, "nameserver", 10) == 0) {
        found = 1; break;
      }
    }
    fclose(f);
    if (found)
      marker("DNS-SMOKE: ok resolv-conf\n");
    else
      marker("DNS-SMOKE: fail resolv-conf\n");
  } else {
    marker("DNS-SMOKE: fail resolv-conf\n");
  }
}

/* FIONREAD and the poll flags a real event-driven server depends on. Both were
 * missing or wrong: the socket ioctl path answered ENODEV for FIONREAD, and a
 * LISTENING unix socket reported POLLHUP because it has no peer — sway's IPC
 * server hit both, dropping every client it accepted. */
static void test_unix_socket_events(void) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) {
    marker("UNIX-SMOKE: fail socketpair\n");
    return;
  }
  int avail = -1;
  if (ioctl(sp[0], FIONREAD, &avail) == 0 && avail == 0) {
    if (write(sp[1], "hello!", 6) == 6) {
      avail = -1;
      if (ioctl(sp[0], FIONREAD, &avail) == 0 && avail == 6)
        marker("UNIX-SMOKE: ok fionread\n");
      else
        marker("UNIX-SMOKE: fail fionread\n");
    } else {
      marker("UNIX-SMOKE: fail fionread\n");
    }
  } else {
    marker("UNIX-SMOKE: fail fionread\n");
  }

  /* A peer that closes IS a hangup, and must be reported as one. */
  close(sp[1]);
  struct pollfd pfd = {sp[0], POLLIN, 0};
  poll(&pfd, 1, 0);
  if (pfd.revents & POLLHUP)
    marker("UNIX-SMOKE: ok peer-close-hup\n");
  else
    marker("UNIX-SMOKE: fail peer-close-hup\n");
  close(sp[0]);

  /* A listening socket has no peer by definition: reporting POLLHUP on it
   * tells an event loop its listening socket died. */
  int srv = socket(AF_UNIX, SOCK_STREAM, 0);
  if (srv < 0) { marker("UNIX-SMOKE: fail listen-socket\n"); return; }
  struct sockaddr_un sun;
  memset(&sun, 0, sizeof(sun));
  sun.sun_family = AF_UNIX;
  strcpy(sun.sun_path, "/tmp/net_smoke_listen.sock");
  unlink(sun.sun_path);
  if (bind(srv, (struct sockaddr *)&sun, sizeof(sun)) < 0 || listen(srv, 4) < 0) {
    marker("UNIX-SMOKE: fail listen-bind\n");
    close(srv);
    return;
  }
  struct pollfd lp = {srv, POLLIN, 0};
  poll(&lp, 1, 0);
  if (lp.revents & POLLHUP) {
    marker("UNIX-SMOKE: fail listen-no-hup\n");
  } else {
    /* ...and once a client is queued it must report readability. */
    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    int connected = 0;
    if (cli >= 0) {
      fcntl(cli, F_SETFL, fcntl(cli, F_GETFL, 0) | O_NONBLOCK);
      connect(cli, (struct sockaddr *)&sun, sizeof(sun));
      connected = 1;
    }
    lp.revents = 0;
    poll(&lp, 1, 100);
    if (connected && (lp.revents & POLLIN) && !(lp.revents & POLLHUP))
      marker("UNIX-SMOKE: ok listen-no-hup\n");
    else
      marker("UNIX-SMOKE: fail listen-no-hup\n");
    if (cli >= 0) close(cli);
  }
  close(srv);
  unlink(sun.sun_path);
}

/* shutdown(2)'s half-close, from the side that has to hear about it.
 *
 * Closing the write half is a statement to the PEER: its read must drain
 * whatever is already queued and then return 0, and its poll must say the
 * socket is readable so an event loop wakes up to find that out. b1nix
 * recorded the flag on the calling socket and told the peer nothing, so both
 * ends sat waiting for each other -- which is how `udevadm control --ping`
 * timed out against a systemd-udevd that had already answered: udevadm shuts
 * its write half and waits for the daemon to close, and the daemon closes when
 * its read returns 0.
 *
 * The reverse direction is checked too, because "the peer sees EOF" must not
 * be got by tearing the connection down: a half-close closes one direction. */
#ifndef POLLRDHUP
#define POLLRDHUP 0x2000
#endif
static void test_unix_half_close(void) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) {
    marker("UNIX-SMOKE: fail halfclose-socketpair\n");
    return;
  }

  /* Queue data, THEN close the write half: the reader must see the bytes
   * before the end of file, not lose them to it. */
  if (write(sp[1], "bye", 3) != 3) {
    marker("UNIX-SMOKE: fail halfclose-write\n");
    close(sp[0]); close(sp[1]);
    return;
  }
  if (shutdown(sp[1], SHUT_WR) < 0) {
    marker("UNIX-SMOKE: fail shutdown-wr-call\n");
    close(sp[0]); close(sp[1]);
    return;
  }

  char buf[8];
  ssize_t n = read(sp[0], buf, sizeof(buf));
  if (n != 3 || memcmp(buf, "bye", 3) != 0) {
    marker("UNIX-SMOKE: fail shutdown-wr-eof\n");
    close(sp[0]); close(sp[1]);
    return;
  }

  /* Readable, and readable for the reason the reader asked about. Polled
   * before the second read, because a loop that is never told the socket is
   * readable never makes that read at all. */
  struct pollfd pfd = {sp[0], POLLIN | POLLRDHUP, 0};
  int pr = poll(&pfd, 1, 1000);
  if (pr == 1 && (pfd.revents & POLLIN) && (pfd.revents & POLLRDHUP))
    marker("UNIX-SMOKE: ok shutdown-wr-poll\n");
  else
    marker("UNIX-SMOKE: fail shutdown-wr-poll\n");

  n = read(sp[0], buf, sizeof(buf));
  if (n == 0)
    marker("UNIX-SMOKE: ok shutdown-wr-eof\n");
  else
    marker("UNIX-SMOKE: fail shutdown-wr-eof\n");

  /* One direction, not both: sp[0] may still write and sp[1] may still read.
   * And sp[1] itself may no longer write -- POSIX says EPIPE. */
  int reverse_ok = 0;
  if (write(sp[0], "back", 4) == 4) {
    char rb[8];
    if (read(sp[1], rb, sizeof(rb)) == 4 && memcmp(rb, "back", 4) == 0)
      reverse_ok = 1;
  }
  /* A write to a socket whose write half is shut raises SIGPIPE as well as
   * reporting EPIPE, and the default disposition would end this test rather
   * than let it report. */
  void (*old_pipe)(int) = signal(SIGPIPE, SIG_IGN);
  errno = 0;
  ssize_t w = write(sp[1], "x", 1);
  int epipe_ok = (w < 0 && errno == EPIPE);
  signal(SIGPIPE, old_pipe);
  if (reverse_ok && epipe_ok)
    marker("UNIX-SMOKE: ok shutdown-wr-oneway\n");
  else
    marker("UNIX-SMOKE: fail shutdown-wr-oneway\n");

  close(sp[0]);
  close(sp[1]);
}

/* SO_RCVTIMEO: a blocking recv with nothing to read must give up at the
 * deadline and report EAGAIN, not wait forever. */
static void test_socket_timeouts(void) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) {
    marker("UNIX-SMOKE: fail timeo-socketpair\n");
    return;
  }
  struct timeval tv = {.tv_sec = 0, .tv_usec = 200000}; /* 200 ms */
  if (setsockopt(sp[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
    marker("UNIX-SMOKE: fail rcvtimeo-set\n");
    close(sp[0]); close(sp[1]);
    return;
  }
  struct timeval got = {0, 0};
  socklen_t glen = sizeof(got);
  if (getsockopt(sp[0], SOL_SOCKET, SO_RCVTIMEO, &got, &glen) != 0 ||
      got.tv_sec != 0 || got.tv_usec != 200000) {
    marker("UNIX-SMOKE: fail rcvtimeo-get\n");
    close(sp[0]); close(sp[1]);
    return;
  }

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  char c;
  ssize_t r = recv(sp[0], &c, 1, 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                           (t1.tv_nsec - t0.tv_nsec) / 1000000);
  /* Must fail with EAGAIN, and must actually have waited — returning
   * immediately would pass the errno check while ignoring the timeout. */
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && elapsed_ms >= 150)
    marker("UNIX-SMOKE: ok rcvtimeo\n");
  else
    marker("UNIX-SMOKE: fail rcvtimeo\n");

  /* With data waiting, the same socket returns it immediately. */
  if (write(sp[1], "x", 1) == 1 && recv(sp[0], &c, 1, 0) == 1 && c == 'x')
    marker("UNIX-SMOKE: ok rcvtimeo-data\n");
  else
    marker("UNIX-SMOKE: fail rcvtimeo-data\n");

  close(sp[0]);
  close(sp[1]);
}

int main(void) {
  test_unix_socket_events();
  test_unix_half_close();
  test_socket_timeouts();
  test_ping_gateway();
  test_udp_send_recv();
  test_tcp_path();
  test_poll_readiness();
  test_dns_parse();
  return 0;
}
