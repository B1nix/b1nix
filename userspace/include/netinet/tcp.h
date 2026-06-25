#ifndef B1NIX_U_NETINET_TCP_H
#define B1NIX_U_NETINET_TCP_H

#include <sys/socket.h>  /* IPPROTO_TCP */

#define TCP_NODELAY  1
#define TCP_MAXSEG   2
#define TCP_CORK     3
#define TCP_KEEPIDLE  4   /* idle seconds before keepalive probes */
#define TCP_KEEPINTVL 5   /* seconds between keepalive probes */
#define TCP_KEEPCNT   6   /* probes before declaring the peer dead */
#define TCP_USER_TIMEOUT 18  /* max time before unacked data forces close (ms) */

#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif

#endif
