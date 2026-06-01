#ifndef B1NIX_U_NETINET_IN_H
#define B1NIX_U_NETINET_IN_H

#include <arpa/inet.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;

#define INADDR_ANY 0x00000000
#define INADDR_LOOPBACK 0x7f000001

#define IN6ADDR_ANY_INIT      { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } }
#define IN6ADDR_LOOPBACK_INIT { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } }

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#define IN6_ARE_ADDR_EQUAL(a, b) \
  (__builtin_memcmp((a)->s6_addr, (b)->s6_addr, 16) == 0)

#endif
