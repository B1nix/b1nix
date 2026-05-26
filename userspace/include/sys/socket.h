#ifndef B1NIX_U_SYS_SOCKET_H
#define B1NIX_U_SYS_SOCKET_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned short sa_family_t;
typedef unsigned int socklen_t;

struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

struct sockaddr_in {
  sa_family_t sin_family;
  unsigned short sin_port;
  unsigned int sin_addr;
  unsigned char sin_zero[8];
};

struct sockaddr_un {
  sa_family_t sun_family;
  char sun_path[108];
};

#define AF_UNIX         1
#define AF_LOCAL        AF_UNIX
#define AF_INET         2

#define SOCK_STREAM     1
#define SOCK_DGRAM      2

int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);

#ifdef __cplusplus
}
#endif

#endif
