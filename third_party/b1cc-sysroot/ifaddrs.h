#ifndef _IFADDRS_H
#define _IFADDRS_H

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* <ifaddrs.h>: network interface address enumeration. b1nix builds this list
 * from the loopback interface (always present) plus the active eth0 (queried
 * via the SIOCGIF* ioctls the kernel implements). Added for the Chromium port
 * (M60-62). */
struct ifaddrs {
  struct ifaddrs  *ifa_next;
  char            *ifa_name;
  unsigned int     ifa_flags;
  struct sockaddr *ifa_addr;
  struct sockaddr *ifa_netmask;
  union {
    struct sockaddr *ifu_broadaddr;
    struct sockaddr *ifu_dstaddr;
  } ifa_ifu;
  void            *ifa_data;
};

#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr

int  getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

#ifdef __cplusplus
}
#endif

#endif /* _IFADDRS_H */
