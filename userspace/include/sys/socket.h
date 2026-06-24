#ifndef B1NIX_U_SYS_SOCKET_H
#define B1NIX_U_SYS_SOCKET_H

#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned short sa_family_t;
typedef unsigned int socklen_t;

struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

struct sockaddr_storage {
  sa_family_t ss_family;
  char __ss_padding[126];
};

#ifndef B1NIX_IN_ADDR_DEFINED
#define B1NIX_IN_ADDR_DEFINED
struct in_addr {
  unsigned int s_addr; /* network byte order */
};
#endif

struct sockaddr_in {
  sa_family_t sin_family;
  unsigned short sin_port;
  struct in_addr sin_addr;
  unsigned char sin_zero[8];
};

struct in6_addr {
  unsigned char s6_addr[16];
};

struct sockaddr_in6 {
  sa_family_t sin6_family;
  unsigned short sin6_port;
  unsigned int sin6_flowinfo;
  struct in6_addr sin6_addr;
  unsigned int sin6_scope_id;
};

struct sockaddr_un {
  sa_family_t sun_family;
  char sun_path[108];
};

struct linger {
  int l_onoff;
  int l_linger;
};

struct msghdr {
  void *msg_name;
  socklen_t msg_namelen;
  struct iovec *msg_iov;
  int msg_iovlen;
  void *msg_control;
  size_t msg_controllen;
  int msg_flags;
};

/* Ancillary data (control messages). Standard Linux/musl layout — provided so
 * recvmsg/sendmsg-with-cmsg consumers (e.g. BusyBox libbb/udp_io.c, which is
 * linked into the nc applet but unused on the basic client/server paths) can
 * compile and walk the control buffer. */
struct cmsghdr {
  size_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & (size_t) ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg) ((unsigned char *)(((struct cmsghdr *)(cmsg)) + 1))
#define CMSG_FIRSTHDR(mhdr) \
  ((size_t)(mhdr)->msg_controllen >= sizeof(struct cmsghdr) \
       ? (struct cmsghdr *)(mhdr)->msg_control \
       : (struct cmsghdr *)0)
#define __CMSG_LEN(cmsg) (((cmsg)->cmsg_len + sizeof(long) - 1) & (size_t) ~(sizeof(long) - 1))
#define __CMSG_NEXT(cmsg) ((unsigned char *)(cmsg) + __CMSG_LEN(cmsg))
#define __MHDR_END(mhdr) ((unsigned char *)(mhdr)->msg_control + (mhdr)->msg_controllen)
#define CMSG_NXTHDR(mhdr, cmsg) \
  ((cmsg)->cmsg_len < sizeof(struct cmsghdr) || \
           __CMSG_LEN(cmsg) + sizeof(struct cmsghdr) >= \
               (size_t)(__MHDR_END(mhdr) - (unsigned char *)(cmsg)) \
       ? (struct cmsghdr *)0 \
       : (struct cmsghdr *)__CMSG_NEXT(cmsg))
#define CMSG_SPACE(len) (CMSG_ALIGN(len) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))

#define SCM_RIGHTS 0x01
#define SCM_CREDENTIALS 0x02

struct ucred {
  int pid;
  unsigned int uid;
  unsigned int gid;
};

#define AF_UNIX         1
#define AF_LOCAL        AF_UNIX
#define AF_INET         2
#define AF_UNSPEC       0
#define AF_INET6        10
#define AF_NETLINK      16
#define AF_ROUTE        AF_NETLINK
#define AF_PACKET       17

#define PF_UNSPEC       AF_UNSPEC
#define PF_UNIX         AF_UNIX
#define PF_LOCAL        AF_LOCAL
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6
#define PF_PACKET       AF_PACKET
#define PF_NETLINK      AF_NETLINK

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_RDM        4
#define SOCK_SEQPACKET  5

/* Type-flag bits OR'd into socket()/accept4() type. Values match Linux so a
 * foreign caller (Rust std's libc FFI) that passes SOCK_CLOEXEC/SOCK_NONBLOCK
 * is interpreted correctly by accept4(). */
#define SOCK_CLOEXEC    02000000
#define SOCK_NONBLOCK   00004000

#define IPPROTO_IP      0
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_BROADCAST    6
#define SO_LINGER       13
#define SO_REUSEPORT    15
#define SO_ACCEPTCONN   30
#define SO_PASSCRED     16
#define SO_PEERCRED     17
#define SO_RCVLOWAT     18
#define SO_SNDLOWAT     19
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_DOMAIN       39
#define SO_PROTOCOL     38

#define SOMAXCONN       4096

#define MSG_OOB         0x01
#define MSG_CTRUNC      0x08
#define MSG_PEEK        0x02
#define MSG_TRUNC       0x20
#define MSG_DONTWAIT    0x40
#define MSG_NOSIGNAL    0x4000

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sv[2]);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
int setsockopt(int sockfd, int level, int optname, const void *optval,
               socklen_t optlen);
int getsockopt(int sockfd, int level, int optname, void *optval,
               socklen_t *optlen);
int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);
int shutdown(int sockfd, int how);

#ifdef __cplusplus
}
#endif

#endif
