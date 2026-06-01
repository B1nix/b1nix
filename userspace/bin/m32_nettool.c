#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_tcp(const char *host, const char *port) {
  struct addrinfo hints, *res = 0;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  int gai = getaddrinfo(host, port, &hints, &res);
  if (gai != 0 || !res) {
    printf("m32-nettool: resolve failed: %s\n", gai_strerror(gai));
    return -1;
  }

  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

static int http_get(int argc, char **argv) {
  if (argc < 4) {
    printf("usage: m32-nettool http-get HOST PORT PATH\n");
    return 1;
  }

  int fd = connect_tcp(argv[1], argv[2]);
  if (fd < 0) {
    printf("m32-nettool: connect failed\n");
    return 1;
  }

  char req[512];
  int n = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.0\r\n"
                   "Host: %s\r\n"
                   "User-Agent: b1nix-m32-nettool/1.0\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   argv[3], argv[1]);
  send(fd, req, (size_t)n, 0);

  char buf[512];
  while ((n = (int)recv(fd, buf, sizeof(buf), 0)) > 0) {
    write(1, buf, (size_t)n);
  }
  close(fd);
  return 0;
}

static int tcp_echo_server(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: m32-nettool tcp-echo PORT\n");
    return 1;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)atoi(argv[1]));
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, 1) < 0) {
    close(fd);
    return 1;
  }

  int cfd = accept(fd, 0, 0);
  if (cfd < 0) {
    close(fd);
    return 1;
  }

  char buf[256];
  int n = (int)recv(cfd, buf, sizeof(buf), 0);
  if (n > 0) send(cfd, buf, (size_t)n, 0);
  close(cfd);
  close(fd);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: m32-nettool http-get|tcp-echo ...\n");
    return 1;
  }
  if (strcmp(argv[1], "http-get") == 0) return http_get(argc - 1, argv + 1);
  if (strcmp(argv[1], "tcp-echo") == 0) return tcp_echo_server(argc - 1, argv + 1);
  printf("m32-nettool: unknown command '%s'\n", argv[1]);
  return 1;
}
