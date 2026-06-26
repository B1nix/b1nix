#ifndef _NETINET_IP6_H
#define _NETINET_IP6_H

/* <netinet/ip6.h> — the IPv6 fixed header (RFC 2460). b1nix's kernel IPv6 path is
 * limited (loopback-only for non-link traffic, see roadmap M84), but the struct
 * is the standard 40-byte layout so userspace networking/test code that builds or
 * parses IPv6 packets (e.g. quiche test support) compiles correctly. */

#include <stdint.h>
#include <netinet/in.h>   /* struct in6_addr */

struct ip6_hdr {
    union {
        struct ip6_hdrctl {
            uint32_t ip6_un1_flow;   /* 4 bits version, 8 bits TC, 20 bits flow-ID */
            uint16_t ip6_un1_plen;   /* payload length */
            uint8_t  ip6_un1_nxt;    /* next header */
            uint8_t  ip6_un1_hlim;   /* hop limit */
        } ip6_un1;
        uint8_t ip6_un2_vfc;         /* 4 bits version, top 4 bits traffic class */
    } ip6_ctlun;
    struct in6_addr ip6_src;         /* source address */
    struct in6_addr ip6_dst;         /* destination address */
};

#define ip6_vfc   ip6_ctlun.ip6_un2_vfc
#define ip6_flow  ip6_ctlun.ip6_un1.ip6_un1_flow
#define ip6_plen  ip6_ctlun.ip6_un1.ip6_un1_plen
#define ip6_nxt   ip6_ctlun.ip6_un1.ip6_un1_nxt
#define ip6_hlim  ip6_ctlun.ip6_un1.ip6_un1_hlim
#define ip6_hops  ip6_ctlun.ip6_un1.ip6_un1_hlim

#endif /* _NETINET_IP6_H */
