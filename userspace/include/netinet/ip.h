#ifndef B1NIX_U_NETINET_IP_H
#define B1NIX_U_NETINET_IP_H

#include <netinet/in.h>

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
