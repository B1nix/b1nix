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

#define IN6ADDR_ANY_INIT      { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } } }
#define IN6ADDR_LOOPBACK_INIT { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } } }

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#define IN6_ARE_ADDR_EQUAL(a, b) \
  (__builtin_memcmp((a)->s6_addr, (b)->s6_addr, 16) == 0)

/* Standard IPv6 address-class tests (glibc <netinet/in.h>). Use the s6_addr32
 * union view + htonl for the word checks; byte view for the prefix checks. */
#define IN6_IS_ADDR_UNSPECIFIED(a) \
  (((const struct in6_addr *)(a))->s6_addr32[0] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[1] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[2] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
  (((const struct in6_addr *)(a))->s6_addr32[0] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[1] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[2] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[3] == htonl(1))
#define IN6_IS_ADDR_MULTICAST(a) \
  (((const struct in6_addr *)(a))->s6_addr[0] == 0xff)
#define IN6_IS_ADDR_LINKLOCAL(a) \
  ((((const struct in6_addr *)(a))->s6_addr[0] == 0xfe) && \
   ((((const struct in6_addr *)(a))->s6_addr[1] & 0xc0) == 0x80))
#define IN6_IS_ADDR_SITELOCAL(a) \
  ((((const struct in6_addr *)(a))->s6_addr[0] == 0xfe) && \
   ((((const struct in6_addr *)(a))->s6_addr[1] & 0xc0) == 0xc0))
#define IN6_IS_ADDR_V4MAPPED(a) \
  (((const struct in6_addr *)(a))->s6_addr32[0] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[1] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[2] == htonl(0xffff))
#define IN6_IS_ADDR_V4COMPAT(a) \
  (((const struct in6_addr *)(a))->s6_addr32[0] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[1] == 0 && \
   ((const struct in6_addr *)(a))->s6_addr32[2] == 0 && \
   ntohl(((const struct in6_addr *)(a))->s6_addr32[3]) > 1)

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
#define IP_PMTUDISC_DONT   0   /* IP_MTU_DISCOVER values */
#define IP_PMTUDISC_WANT   1
#define IP_PMTUDISC_DO     2
#define IP_PMTUDISC_PROBE  3
#define IP_RECVTTL         12
#define IP_RECVTOS         13
#define IP_MULTICAST_IF    32
#define IP_MULTICAST_TTL   33
#define IP_MULTICAST_LOOP  34
#define IP_ADD_MEMBERSHIP  35
#define IP_DROP_MEMBERSHIP 36
#define IP_DEFAULT_MULTICAST_TTL  1
#define IP_DEFAULT_MULTICAST_LOOP 1

/* IPPROTO_IPV6-level options (Linux ABI). */
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP     20
#define IPV6_LEAVE_GROUP    21
#define IPV6_MTU_DISCOVER  23
#define IPV6_V6ONLY        26
#define IPV6_RECVTCLASS    66
#define IPV6_TCLASS        67
#define IPV6_PMTUDISC_DONT  0   /* IPV6_MTU_DISCOVER values */
#define IPV6_PMTUDISC_WANT  1
#define IPV6_PMTUDISC_DO    2
#define IPV6_PMTUDISC_PROBE 3

/* Protocol-independent multicast group ops (Linux ABI). */
#define MCAST_JOIN_GROUP         42
#define MCAST_BLOCK_SOURCE       43
#define MCAST_UNBLOCK_SOURCE     44
#define MCAST_LEAVE_GROUP        45
#define MCAST_JOIN_SOURCE_GROUP  46
#define MCAST_LEAVE_SOURCE_GROUP 47

/* Multicast membership request structs (Linux layout). */
struct ip_mreq {
  struct in_addr imr_multiaddr;
  struct in_addr imr_interface;
};
struct ip_mreqn {
  struct in_addr imr_multiaddr;
  struct in_addr imr_address;
  int imr_ifindex;
};
struct ipv6_mreq {
  struct in6_addr ipv6mr_multiaddr;
  unsigned int ipv6mr_interface;
};
struct group_req {
  uint32_t gr_interface;
  struct sockaddr_storage gr_group;
};
struct group_source_req {
  uint32_t gsr_interface;
  struct sockaddr_storage gsr_group;
  struct sockaddr_storage gsr_source;
};

#endif
