#ifndef B1NIX_U_ARPA_INET_H
#define B1NIX_U_ARPA_INET_H

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct in_addr {
  unsigned int s_addr; /* network byte order */
};

/* x86_64 is little-endian, so host<->network is a byte swap. */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
  return ((x & 0xFFu) << 24) | ((x & 0xFF00u) << 8) | ((x >> 8) & 0xFF00u) |
         ((x >> 24) & 0xFFu);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* Dotted-quad <-> binary helpers. Addresses are kept in network byte order:
 * for a.b.c.d the in-memory bytes are {a,b,c,d}, i.e. s_addr == a | b<<8 |
 * c<<16 | d<<24 on a little-endian host (matching the b1nix socket ABI). */
unsigned int inet_addr(const char *cp);
int inet_aton(const char *cp, struct in_addr *inp);
char *inet_ntoa(struct in_addr in);
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#ifdef __cplusplus
}
#endif

#endif
