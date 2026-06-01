#ifndef B1NIX_U_NETINET_IN_H
#define B1NIX_U_NETINET_IN_H

#include <arpa/inet.h>
#include <sys/socket.h>

#define IN6ADDR_ANY_INIT      { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } }
#define IN6ADDR_LOOPBACK_INIT { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } }

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#endif
