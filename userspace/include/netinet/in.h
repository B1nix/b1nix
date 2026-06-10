#ifndef B1NIX_U_NETINET_IN_H
#define B1NIX_U_NETINET_IN_H

#include <arpa/inet.h>
#include <sys/socket.h>

struct in_pktinfo {
  int ipi_ifindex;
  struct in_addr ipi_spec_dst;
  struct in_addr ipi_addr;
};

typedef uint32_t in_addr_t;

#define INADDR_ANY 0x00000000
#define INADDR_LOOPBACK 0x7f000001
#define INADDR_NONE 0xffffffff
#define INADDR_BROADCAST 0xffffffff

#define IPPORT_RESERVED 1024

#define IN6ADDR_ANY_INIT      { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } }
#define IN6ADDR_LOOPBACK_INIT { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } }

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#define IN6_ARE_ADDR_EQUAL(a, b) \
  (__builtin_memcmp((a)->s6_addr, (b)->s6_addr, 16) == 0)

/* IPPROTO_IP-level socket options (Linux ABI). b1nix does not act on most of
 * these, but the symbols must exist for BusyBox ping/traceroute to compile;
 * the kernel setsockopt ignores unknown IP-level options. */
#ifndef IP_TOS
#define IP_TOS             1
#endif
#ifndef IP_TTL
#define IP_TTL             2
#endif
#define IP_HDRINCL         3
#define IP_OPTIONS         4
#define IP_ROUTER_ALERT    5
#define IP_RECVOPTS        6
#define IP_PKTINFO         8
#define IP_MTU_DISCOVER    10
#define IP_RECVTTL         12
#define IP_MULTICAST_IF    32
#define IP_MULTICAST_TTL   33
#define IP_MULTICAST_LOOP  34
#define IP_ADD_MEMBERSHIP  35
#define IP_DROP_MEMBERSHIP 36

#endif
