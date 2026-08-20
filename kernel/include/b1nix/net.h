#ifndef B1NIX_NET_H
#define B1NIX_NET_H

#include <b1nix/types.h>

struct mac_addr {
	u8 bytes[6];
};

struct ipv4_addr {
	u8 bytes[4];
};

struct in6_addr_k {
	u8 bytes[16];
};

void net_init(void);
void net_poll(void);
/* Loopback (127.0.0.0/8, ::1) datagrams are NOT delivered synchronously inside
 * the sender's call stack — that recursion (ipv4_send -> ipv4_receive ->
 * tcp_input -> tcp_send -> ipv4_send -> ...) re-enters the TCP state machine
 * mid-send and deadlocks multi-packet exchanges such as an SSH handshake.
 * Instead the IP layer enqueues the finished datagram and net_poll()/net_task
 * drains it in a clean context. is_v6 picks ipv4_receive vs ipv6_receive. */
void net_loopback_enqueue(const void *ip_pkt, usize len, int is_v6);
void net_loopback_drain(void);
int net_is_ready(void);
void net_dump_info(void);
void net_interrupt_handler(void);
/* Acknowledge every registered interface sitting on `irq` and wake net_task if
 * any of them claimed the interrupt. Returns 1 when claimed. */
int net_handle_irq(int irq);
int net_get_irq(void);

// Virtio Network Data Plane
struct netdev;
void net_send_ethernet(struct mac_addr dst, u16 ethertype, const void *payload, usize size);
/* Same, but transmitted out of a specific interface (NULL = the active one).
 * Used by the per-interface FIB so a route's output device is honoured. */
void net_send_ethernet_dev(struct netdev *nd, struct mac_addr dst, u16 ethertype,
                           const void *payload, usize size);
/* Same, carrying per-packet driver requests (NETDEV_TX_F_*). */
void net_send_ethernet_tx(struct netdev *nd, struct mac_addr dst, u16 ethertype,
                          const void *payload, usize size, u32 tx_flags);
/* Interface indices: 1-based registration order, 0 = unspecified. */
int netdev_index_of(struct netdev *nd);
struct netdev *netdev_by_index(int idx);
void netdev_ifname(int idx, char *out, usize cap);
int netdev_index_by_name(const char *name);
/* Administrative state. netdev_set_admin_up() takes the interface up or down
 * the way `ip link set <if> up/down` does: down stops its transmit and receive
 * paths and, if it was the active L3 interface, hands that role to another one
 * (or leaves the stack with no active interface). Returns 0, or -ENODEV. */
int netdev_set_admin_up(struct netdev *nd, int up);
int netdev_is_admin_up(const struct netdev *nd);

/*
 * Per-packet receive-path flags.
 *
 * NET_RX_F_CSUM_OK says the L4 (TCP/UDP) checksum needs no software check:
 * either the NIC validated it (VIRTIO_NET_F_GUEST_CSUM and friends), or the
 * packet never crossed a wire at all (loopback, or a segment injected straight
 * into the state machine by a self-test). Everything without this flag is
 * verified in software by ipv4_receive_flags() and dropped when it is wrong.
 */
#define NET_RX_F_CSUM_OK 0x1u

// Ethernet
void ethernet_receive(const void *data, usize size);
void ethernet_receive_flags(const void *data, usize size, u32 rx_flags);

// ARP
void arp_init(void);
/* Drop every neighbour a departing network namespace had learned. */
void arp_flush_ns(u32 ns);
void arp_receive(const void *data, usize size);
int arp_resolve(struct ipv4_addr ip, struct mac_addr *mac);
/* Resolve, sending any request out of `dev` (NULL = the active interface). */
int arp_resolve_dev(struct ipv4_addr ip, struct mac_addr *mac,
                    struct netdev *dev);

/* M107: neighbour-table enumeration and administration, shared by the IPv4 ARP
 * cache. (The IPv6 NDP cache has no enumeration API yet, so `ip -6 neigh`
 * reports an empty table rather than a fabricated one.) `permanent` marks an
 * entry installed by hand (`ip neigh add`) rather than learned from the wire:
 * it reports NUD_PERMANENT and is never overwritten by a learned reply. */
struct neigh_info {
	u8 family; /* B1NIX_AF_INET or B1NIX_AF_INET6 */
	u8 addr[16];
	u8 addr_len; /* 4 or 16 */
	u8 permanent;
	struct mac_addr mac;
	int oif;
};

usize arp_snapshot(struct neigh_info *out, usize max);
/* Install (or replace) a neighbour entry. `permanent` != 0 pins it. */
int arp_neigh_set(struct ipv4_addr ip, struct mac_addr mac, int permanent);
/* Remove one entry; 0 on success, -ESRCH when the address is not cached. */
int arp_neigh_del(struct ipv4_addr ip);

// IPv4
void ipv4_receive(const void *data, usize size);
void ipv4_receive_flags(const void *data, usize size, u32 rx_flags);
/* Number of datagrams dropped because their TCP/UDP checksum did not verify. */
u64 ipv4_rx_csum_errors(void);
void ipv4_send(struct ipv4_addr dst, u8 protocol, const void *payload, usize size);

/*
 * The datagram's TCP/UDP checksum field is left zero and the IP layer fills it
 * in. Only the IP layer knows the final source address (it rewrites it for
 * loopback) and which interface the FIB picked, and only there can the choice
 * between "compute it here" and "hand the NIC a partial sum to finish" be made
 * per packet. A sender that builds its own complete L4 checksum — a raw socket,
 * ICMP — simply does not pass this flag.
 */
#define IPV4_TX_F_CSUM_L4 0x1u

/* ipv4_send() plus the per-packet flags above. */
void ipv4_send_tx(struct ipv4_addr dst, u8 protocol, const void *payload,
                  usize size, u32 ip_tx_flags);
int ipv4_is_loopback(struct ipv4_addr ip);

// IPv6 datapath (loopback + real-link via NDP)
void ipv6_receive(const void *data, usize size);
void ipv6_send(struct in6_addr_k dst, u8 next_header, const void *payload, usize size);
/* Same, transmitted out of a specific interface (NULL = let the FIB decide). */
void ipv6_send_via(struct netdev *dev, struct in6_addr_k dst, u8 next_header,
                   const void *payload, usize size);
u32 icmpv6_echo_reply_count(void);
void icmpv6_send_dest_unreachable(struct in6_addr_k dst, u8 code,
                                  const void *quoted, usize quoted_len);

// IPv6 interface state
struct in6_addr_k net_get_ip6_ll(void);
struct in6_addr_k net_get_ip6(void);
struct in6_addr_k net_get_gateway6(void);
struct in6_addr_k net_get_prefix6(void);
int net_get_prefix6_valid(void);
void net_set_ip6(struct in6_addr_k a);
void net_set_gateway6(struct in6_addr_k a);
void net_set_prefix6(struct in6_addr_k p);

// Neighbor Discovery + SLAAC (kernel/net/ndp.c)
void ndp_init(void);
void ndp_tick(u64 now_ticks);
void mld_join_solicited_node(struct in6_addr_k address);
void mld_receive(struct in6_addr_k src, struct in6_addr_k dst, u8 type,
                 const void *data, usize size);
void mld_smoke(void);
/* Handle an incoming ICMPv6 Neighbor Discovery message (RS/RA/NS/NA),
 * dispatched from ipv6_receive for types 133-136. */
void ndp_receive(struct in6_addr_k src, struct in6_addr_k dst, u8 type,
                 const void *data, usize size);
/* Resolve a link-local/on-link IPv6 address to a MAC. Returns 1 with *mac on a
 * cache hit, otherwise sends a Neighbor Solicitation and returns 0 (retry). */
int ndp_resolve(struct in6_addr_k ip, struct mac_addr *mac);
/* Resolve, soliciting out of `dev` (NULL = the active interface). */
int ndp_resolve_dev(struct in6_addr_k ip, struct mac_addr *mac,
                    struct netdev *dev);
/* UDP over IPv6 (loopback ::1 datapath). */
void udp6_send(struct in6_addr_k dst, u16 src_port_net, u16 dst_port_net,
               const void *payload, usize size);
void udp6_receive(struct in6_addr_k src, struct in6_addr_k dst,
                  const void *data, usize size);
/* Offline self-test: ping ::1 through the loopback datapath and verify an
 * ICMPv6 echo reply comes back. Emits an M32-IP6 marker. */
void ipv6_loopback_smoke(void);
/* Real-link self-test: SLAAC + an ICMPv6 ping of the usernet IPv6 gateway. */
void ipv6_realink_smoke(void);

// ICMP
void icmp_receive(struct ipv4_addr src, const void *data, usize size);
void icmp_send_dest_unreachable(struct ipv4_addr dst, u8 code);
u32 icmp_echo_reply_count(void);

// UDP
typedef void (*udp_port_handler_t)(const void *data, usize size);
void udp_receive(struct ipv4_addr src, const void *data, usize size);
void udp_send_net(struct ipv4_addr dst, u16 src_port_net, u16 dst_port_net, const void *payload, usize size);
void udp_send(struct ipv4_addr dst, u16 src_port, u16 dst_port, const void *payload, usize size);
int udp_register_handler(u16 port, udp_port_handler_t handler);
/* IPv6 UDP handlers also receive the datagram's source address. */
typedef void (*udp6_port_handler_t)(struct in6_addr_k src, const void *data,
                                    usize size);
int udp6_register_handler(u16 port, udp6_port_handler_t handler);
/* Drop a port handler. A protocol module that registered one must call this
 * from its exit path, or a later datagram jumps into freed module text. */
void udp_unregister_handler(u16 port);

// DHCP
void dhcp_init(void);
void dhcp_receive(const void *data, usize size);
void dhcp_tick(u64 now_ticks);
void dhcp_dump_info(void);
void dhcp_stop(void);
/* Called after repeated DHCP failures. Returns 1 when another interface was
 * selected and DHCP was restarted there. */
int net_dhcp_try_failover(void);

// NTP (SNTP client)
void ntp_tick(u64 now_ticks);

// DNS
void dns_resolve(const char *domain);
void dns_receive(const void *data, usize size);
/* Synchronous resolve: send a query and poll the network until an A record
 * arrives or a short timeout elapses. Returns 0 and fills out[4] on success,
 * -1 on timeout/failure. */
int dns_resolve_sync(const char *domain, u8 out[4]);
/* Internal-service variant: same lookup without console diagnostics. */
int dns_resolve_sync_quiet(const char *domain, u8 out[4]);
/* Last A-record result captured by dns_receive (1 if available, fills out). */
int dns_last_result(u8 out[4]);
/* Last AAAA-record result captured by dns_receive (1 if available, fills 16). */
int dns_last_result6(u8 out[16]);
/* Configure / read the resolver's nameserver (also loaded from
 * /etc/resolv.conf lazily on first use). */
void dns_set_server(struct ipv4_addr server);
struct ipv4_addr dns_get_server(void);
/* IPv6 nameserver learnt from DHCPv6 option 23. */
void dns_set_server6(struct in6_addr_k s);
struct in6_addr_k dns_get_server6(void);
int dns_has_server6(void);

// DHCPv6 (RFC 8415) — stateful address configuration
void dhcpv6_init(void);
void dhcpv6_start(void);
void dhcpv6_stop(void);
void dhcpv6_tick(u64 now_ticks);
int dhcpv6_is_bound(void);
struct in6_addr_k dhcpv6_get_address(void);
/* Self-test: drives Solicit/Advertise/Request/Reply and Renew through the real
 * receive path against a synthetic server. */
void dhcpv6_smoke(void);
/* Lazily parse /etc/resolv.conf for "nameserver <ip>"; returns 1 if a server
 * was parsed, 0 otherwise. Idempotent. */
int dns_load_resolv_conf(void);

// TCP
void tcp_receive(struct ipv4_addr src, const void *data, usize size);
struct tcp_conn;
struct tcp_conn *tcp_connect(struct ipv4_addr dst_ip, u16 dst_port);
struct tcp_conn *tcp_connect_async(struct ipv4_addr dst_ip, u16 dst_port);
int tcp_is_established(struct tcp_conn *conn);
int tcp_is_readable(struct tcp_conn *conn);
usize tcp_bytes_available(struct tcp_conn *conn);
int tcp_is_close_wait(struct tcp_conn *conn);
int tcp_is_closed(struct tcp_conn *conn);
int tcp_send(struct tcp_conn *conn, const void *data, usize len);
/* SO_RCVBUF for an established connection: caps the receive buffer and the
 * window advertised from it, and stops the buffer auto-tuning past that. */
void tcp_set_rcvbuf(struct tcp_conn *conn, u32 bytes);
/* Keepalive. `which` selects the parameter: 0 = idle, 1 = interval, 2 = count,
 * matching TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT. Seconds, as
 * setsockopt(2) takes them. */
void tcp_set_keepalive(struct tcp_conn *conn, int on);
int tcp_set_keepalive_param(struct tcp_conn *conn, int which, u32 seconds);
u32 tcp_get_keepalive_param(struct tcp_conn *conn, int which);
int tcp_recv(struct tcp_conn *conn, void *buf, usize max_len, int flags);
int tcp_close(struct tcp_conn *conn);
/* Claim a connection slot for a listener (or 0 when the pool is exhausted). The
 * returned conn is stored on the socket state so a later close() reclaims it. */
struct tcp_conn *tcp_listen(u16 local_port, int backlog);
struct tcp_conn *tcp_accept(u16 local_port, struct ipv4_addr *client_ip, u16 *client_port);
/* Socket-table snapshot for /proc/net/{tcp,udp} (netstat). Fills up to `max`
 * entries and returns the count written. `state` carries the Linux
 * /proc/net/tcp st code (0x0A = LISTEN, 0x01 = ESTABLISHED, ...); 0 for UDP. */
struct net_sock_info {
	u8 family;          /* 4 = IPv4, 6 = IPv6 */
	u8 local_ip[16];
	u8 remote_ip[16];
	u16 local_port;
	u16 remote_port;
	int state;
};
usize tcp_conn_snapshot(struct net_sock_info *out, usize max);
usize udp_binding_snapshot(struct net_sock_info *out, usize max);
/* Deliver an inbound ICMP packet to every SOCK_RAW/ICMP socket (BusyBox ping),
 * prepending a synthetic IPv4 header. Called from icmp_receive(). */
void vfs_socket_push_raw_icmp(struct ipv4_addr src, const void *icmp, usize len);
/* TCP over IPv6 (loopback ::1). */
void tcp6_receive(struct in6_addr_k src, const void *data, usize size);
struct tcp_conn *tcp_connect6(struct in6_addr_k dst_ip6, u16 dst_port);
struct tcp_conn *tcp_accept6(u16 local_port, struct in6_addr_k *client_ip6, u16 *client_port);
int tcp_pending_connections(u16 local_port);
int tcp_network_ready(void);
void tcp_timer_tick(void);
u32 tcp_debug_peek_iss(struct ipv4_addr remote_ip, u16 remote_port,
                       u16 local_port);

// Network state
struct mac_addr net_get_mac(void);
struct ipv4_addr net_get_ip(void);
struct ipv4_addr net_get_gateway(void);
struct ipv4_addr net_get_netmask(void);
void net_set_ip(struct ipv4_addr ip);
void net_set_gateway(struct ipv4_addr gw);
void net_set_netmask(struct ipv4_addr mask);

/* ── M84: IPv4 FIB (kernel/net/route.c) ──────────────────────────────────
 * Longest-prefix-match routing table. Addresses are host order (a.b.c.d ->
 * 0xAABBCCDD); use route_ipv4_to_host()/route_host_to_ipv4() at the edges.
 * Flag values match Linux's RTF_* so BusyBox `route` and /proc/net/route
 * agree with the kernel. */
#define RTF_UP      0x0001
#define RTF_GATEWAY 0x0002
#define RTF_HOST    0x0004

struct route_info {
	u32 dst;
	u32 mask;
	u32 gateway;
	u16 flags;
	u16 metric;
	int oif;         /* output interface index, 0 = unspecified */
	u32 table;
	char iface[8];
};

struct route6_info {
	u8 dst[16];
	u8 gateway[16];
	u8 plen;
	u16 flags;
	u16 metric;
	int oif;
	u32 table;
	char iface[8];
};

/* Policy routing: routes live in numbered tables, rules pick the table. */
#define RT_TABLE_MAIN 254

struct route_rule_info {
	u8 family;
	u32 prio;
	u32 src;
	u32 src_mask;
	u8 src6[16];
	u8 src6_plen;
	int iif;
	u32 table;
};

u32 route_ipv4_to_host(struct ipv4_addr a);
struct ipv4_addr route_host_to_ipv4(u32 v);
int route_add(u32 dst, u32 mask, u32 gw, u16 flags, u16 metric,
              const char *iface);
/* Same, naming the output interface by index (0 = unspecified). */
int route_add_oif(u32 dst, u32 mask, u32 gw, u16 flags, u16 metric, int oif);
int route_add_table(u32 dst, u32 mask, u32 gw, u16 flags, u16 metric, int oif,
                    u32 table);
int route_del(u32 dst, u32 mask, u32 gw);
int route_del_table(u32 dst, u32 mask, u32 gw, u32 table);
/* Install the default policy (everything looks in the main table). */
void route_init(void);
void route_rules_reset(void);
int route_rule_add(u8 family, u32 prio, struct ipv4_addr src4,
                   struct ipv4_addr mask4, struct in6_addr_k src6, u8 plen6,
                   int iif, u32 table);
int route_rule_del(u8 family, u32 prio);
usize route_rule_snapshot(struct route_rule_info *out, usize max);
/* Text control planes behind /proc/net/rt_tables and /proc/net/rt_rules. */
int route_control_write(const char *buf, usize len);
int route_rule_control_write(const char *buf, usize len);
/* Drop routes installed by DHCP/autoconf; manual routes survive. */
void route_flush_dynamic(void);
void route_flush_all(void);
/* Drop every route (v4 and v6) of a departing network namespace. */
void route_flush_ns(u32 ns);
/* Install the on-link prefix for `ip`/`mask` plus a default route via `gw`,
 * replacing any previous dynamic entries. Called on every DHCP bind. */
void route_configure_interface(struct ipv4_addr ip, struct ipv4_addr mask,
                               struct ipv4_addr gw);
/* Longest-prefix-match, with equal-cost load sharing hashed on the
 * destination. Returns 1 and fills *nexthop with the address to resolve at
 * layer 2 (the destination for an on-link route, the gateway otherwise), the
 * matched route's flags, and its output interface index. 0 when no route
 * matches. Any out-parameter may be NULL. */
int route_lookup(struct ipv4_addr dst, struct ipv4_addr *nexthop, u16 *flags,
                 int *oif);
/* Same, but the equal-cost hop is chosen by a flow hash (5-tuple) so distinct
 * flows to one destination can use different next hops without reordering. */
int route_lookup_flow(struct ipv4_addr dst, u32 flow, struct ipv4_addr *nexthop,
                      u16 *flags, int *oif);
/* Full policy lookup: source and input interface select the rule, the rule
 * selects the table. */
int route_lookup_ex(struct ipv4_addr src, struct ipv4_addr dst, u32 flow,
                    int iif, struct ipv4_addr *nexthop, u16 *flags, int *oif);
/* Flow hash over the 5-tuple, for the ECMP selector. */
u32 route_flow_hash(const void *src_addr, const void *dst_addr, usize addr_len,
                    u8 proto, u16 sport, u16 dport);
usize route_snapshot(struct route_info *out, usize max);

/* IPv6 FIB — same model, prefix lengths instead of masks. */
void route6_init(void);
int route6_add(struct in6_addr_k dst, u8 plen, struct in6_addr_k gw, u16 flags,
               u16 metric, int oif);
int route6_add_table(struct in6_addr_k dst, u8 plen, struct in6_addr_k gw,
                     u16 flags, u16 metric, int oif, u32 table);
int route6_del(struct in6_addr_k dst, u8 plen, struct in6_addr_k gw);
int route6_del_table(struct in6_addr_k dst, u8 plen, struct in6_addr_k gw,
                     u32 table);
void route6_flush_dynamic(void);
void route6_flush_all(void);
/* SLAAC result: on-link prefix + default route via the advertising router. */
void route6_configure_interface(struct in6_addr_k prefix, u8 plen,
                                struct in6_addr_k router);
int route6_lookup(struct in6_addr_k dst, struct in6_addr_k *nexthop,
                  u16 *flags, int *oif);
int route6_lookup_flow(struct in6_addr_k dst, u32 flow,
                       struct in6_addr_k *nexthop, u16 *flags, int *oif);
int route6_lookup_ex(struct in6_addr_k src, struct in6_addr_k dst, u32 flow,
                     int iif, struct in6_addr_k *nexthop, u16 *flags, int *oif);
usize route6_snapshot(struct route6_info *out, usize max);
/* M84 self-test: LPM ordering, host routes, metric tie-break, deletion. */
void route_smoke(void);
/* M84 self-test: TCP option parsing, window scaling, out-of-order reassembly. */
void tcp_robustness_smoke(void);
 
struct vfs_socket_state;
struct vfs_handle;
struct b1nix_ucred;
struct b1nix_sockaddr_un;
struct b1nix_pollfd;

// UNIX Domain Sockets (Internal)
int unix_init_state(struct vfs_socket_state *s);
void unix_free_state(struct vfs_socket_state *s);
/* M57: cross-connect two initialised AF_UNIX states as connected peers
 * (socketpair, no filesystem name). */
void unix_link_pair(struct vfs_socket_state *a, struct vfs_socket_state *b);
int unix_bind(struct vfs_socket_state *s, const struct b1nix_sockaddr_un *addr);
int unix_listen(struct vfs_socket_state *s, int backlog);
int unix_connect(struct vfs_socket_state *s, const struct b1nix_sockaddr_un *addr,
                 int nonblock);
int unix_accept(struct vfs_socket_state *s, struct vfs_socket_state *new_s,
                int nonblock);
isize unix_send(struct vfs_socket_state *s, const void *buf, usize len,
                int nonblock);
isize unix_recv(struct vfs_socket_state *s, void *buf, usize len);
isize unix_send_control(struct vfs_socket_state *s, const void *buf, usize len,
                        struct vfs_handle **handles, usize nhandles,
                        const struct b1nix_ucred *cred, int nonblock);
isize unix_recv_control(struct vfs_socket_state *s, void *buf, usize len,
                        int flags, struct vfs_handle **handles,
                        usize *nhandles, struct b1nix_ucred *cred,
                        int *has_cred);
int unix_poll(struct vfs_socket_state *s, struct b1nix_pollfd *pfd);
/* SO_PEERCRED — credentials of the peer socket's creator. -ENOTCONN if the
 * socket has no peer. */
int unix_peer_cred(struct vfs_socket_state *s, struct b1nix_ucred *out);
usize unix_bytes_available(struct vfs_socket_state *s);

#endif
