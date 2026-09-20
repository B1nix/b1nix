#ifndef _NETINET_UDP_H
#define _NETINET_UDP_H

/* <netinet/udp.h> — the 8-byte UDP header (RFC 768). b1nix's kernel UDP path
 * doesn't use this userspace layout, but packet-building/parsing code (e.g.
 * quiche test support) needs the struct. Anonymous union exposes both the BSD
 * (uh_*) and glibc (source/dest/len/check) field names. */

#include <stdint.h>

struct udphdr {
    union {
        struct {
            uint16_t uh_sport;   /* source port */
            uint16_t uh_dport;   /* destination port */
            uint16_t uh_ulen;    /* udp length */
            uint16_t uh_sum;     /* udp checksum */
        };
        struct {
            uint16_t source;
            uint16_t dest;
            uint16_t len;
            uint16_t check;
        };
    };
};

#endif /* _NETINET_UDP_H */
