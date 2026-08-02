/* Network protocol registration (M96).
 *
 * The core stack (ethernet demux, the net daemon's tick, the loopback drain)
 * no longer calls IPv6/NDP/NTP directly: those live in loadable modules and
 * publish themselves here. Every hook is optional — a kernel running without
 * ipv6.ko simply has no protocol claiming ETHERTYPE_IPV6, and an IPv6 send
 * from TCP/UDP reports "no route" instead of jumping into unmapped memory.
 */

#ifndef B1NIX_NETPROTO_H
#define B1NIX_NETPROTO_H

#include <b1nix/net.h>
#include <b1nix/types.h>

struct net_proto {
  const char *name;
  /* Non-zero: this protocol claims the given EtherType from the L2 demux. */
  u16 ether_type;

  /* L3 receive for `ether_type` frames (payload only, header stripped). */
  void (*receive)(const void *data, usize size);
  /* IPv6 transmit path used by TCP/UDP. */
  void (*send6)(struct in6_addr_k dst, u8 next_header, const void *payload,
                usize size);
  /* Neighbour resolution: 1 on a cache hit (fills *mac), 0 when a solicitation
   * was sent and the caller should retry. */
  int (*resolve6)(struct in6_addr_k ip, struct mac_addr *mac);
  /* ICMPv6 Neighbour Discovery types 133-136. */
  void (*icmp6)(struct in6_addr_k src, struct in6_addr_k dst, u8 type,
                const void *data, usize size);
  /* ICMPv6 destination-unreachable, sent by the UDP layer for a datagram
   * that no socket claimed. */
  void (*icmp6_unreach)(struct in6_addr_k dst, u8 code, const void *quoted,
                        usize quoted_len);
  /* Periodic work, driven by the net daemon at ~100 Hz. */
  void (*tick)(u64 now_ticks);
  /* Interface reconfigured (address change / adapter switch). */
  void (*reset)(void);
  /* b1nix.test=1 self-test. */
  void (*selftest)(void);

  struct net_proto *next;
};

/* Register/withdraw a protocol. Registering the same struct twice, or a name
 * that is already present, returns -EEXIST. */
int proto_register(struct net_proto *proto);
void proto_unregister(struct net_proto *proto);
struct net_proto *proto_find(const char *name);
usize proto_count(void);
/* Name of the i-th registered protocol, or NULL. Used by /proc/net/protocols. */
const char *proto_name_at(usize index);

/* Core-side dispatch, all no-ops when no module provides the hook. */
int proto_deliver_ether(u16 ether_type, const void *data, usize size);
void net_proto_tick(u64 now_ticks);
void net_proto_reset(void);
void net_proto_selftest(void);
/* 1 when some protocol can transmit IPv6. */
int net_proto_ipv6_available(void);
void net_proto_ipv6_send(struct in6_addr_k dst, u8 next_header,
                         const void *payload, usize size);
void net_proto_icmp6_unreach(struct in6_addr_k dst, u8 code, const void *quoted,
                             usize quoted_len);
/* ND hooks, called from the IPv6 module. */
void ndp_dispatch_receive(struct in6_addr_k src, struct in6_addr_k dst, u8 type,
                          const void *data, usize size);
int ndp_dispatch_resolve(struct in6_addr_k ip, struct mac_addr *mac);

#endif /* B1NIX_NETPROTO_H */
