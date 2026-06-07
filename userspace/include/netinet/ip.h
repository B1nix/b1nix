#ifndef B1NIX_U_NETINET_IP_H
#define B1NIX_U_NETINET_IP_H

#include <stdint.h>
#include <endian.h>
#include <netinet/in.h>

/* Linux struct iphdr, as BusyBox ping expects from the system headers on a
 * non-FreeBSD build (it parses the IP header of a SOCK_RAW reply via
 * iphdr->ihl). b1nix is little-endian on both supported arches. */
struct iphdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
  unsigned int ihl : 4;
  unsigned int version : 4;
#else
  unsigned int version : 4;
  unsigned int ihl : 4;
#endif
  uint8_t tos;
  uint16_t tot_len;
  uint16_t id;
  uint16_t frag_off;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t check;
  uint32_t saddr;
  uint32_t daddr;
};

/* IP type-of-service / DSCP bits used by sshd to mark interactive vs bulk
 * traffic via setsockopt(IP_TOS). b1nix does not act on these, but the symbols
 * must exist for the port to compile. */
#define IPTOS_LOWDELAY     0x10
#define IPTOS_THROUGHPUT   0x08
#define IPTOS_RELIABILITY  0x04
#define IPTOS_MINCOST      0x02

#ifndef IPTOS_DSCP_AF21
#define IPTOS_DSCP_AF21    0x48
#endif
#ifndef IPTOS_DSCP_EF
#define IPTOS_DSCP_EF      0xb8
#endif

#endif
