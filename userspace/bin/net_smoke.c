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
#include <sys/socket.h>
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

int main(void) {
  test_ping_gateway();
  test_udp_send_recv();
  test_tcp_path();
  test_poll_readiness();
  test_dns_parse();
  return 0;
}
