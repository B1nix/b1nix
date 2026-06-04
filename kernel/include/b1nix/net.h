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
int net_get_irq(void);

// Virtio Network Data Plane
void net_send_ethernet(struct mac_addr dst, u16 ethertype, const void *payload, usize size);

// Ethernet
void ethernet_receive(const void *data, usize size);

// ARP
void arp_init(void);
void arp_receive(const void *data, usize size);
int arp_resolve(struct ipv4_addr ip, struct mac_addr *mac);

// IPv4
void ipv4_receive(const void *data, usize size);
void ipv4_send(struct ipv4_addr dst, u8 protocol, const void *payload, usize size);

// IPv6 datapath (loopback + real-link via NDP)
void ipv6_receive(const void *data, usize size);
void ipv6_send(struct in6_addr_k dst, u8 next_header, const void *payload, usize size);
u32 icmpv6_echo_reply_count(void);

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
/* Handle an incoming ICMPv6 Neighbor Discovery message (RS/RA/NS/NA),
 * dispatched from ipv6_receive for types 133-136. */
void ndp_receive(struct in6_addr_k src, struct in6_addr_k dst, u8 type,
                 const void *data, usize size);
/* Resolve a link-local/on-link IPv6 address to a MAC. Returns 1 with *mac on a
 * cache hit, otherwise sends a Neighbor Solicitation and returns 0 (retry). */
int ndp_resolve(struct in6_addr_k ip, struct mac_addr *mac);
/* UDP over IPv6 (loopback ::1 datapath). */
void udp6_send(struct in6_addr_k dst, u16 src_port_net, u16 dst_port_net,
               const void *payload, usize size);
void udp6_receive(struct in6_addr_k src, const void *data, usize size);
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

// DHCP
void dhcp_init(void);
void dhcp_receive(const void *data, usize size);
void dhcp_tick(u64 now_ticks);
void dhcp_dump_info(void);

// NTP (SNTP client)
void ntp_tick(u64 now_ticks);

// DNS
void dns_resolve(const char *domain);
void dns_receive(const void *data, usize size);
/* Synchronous resolve: send a query and poll the network until an A record
 * arrives or a short timeout elapses. Returns 0 and fills out[4] on success,
 * -1 on timeout/failure. */
int dns_resolve_sync(const char *domain, u8 out[4]);
/* Last A-record result captured by dns_receive (1 if available, fills out). */
int dns_last_result(u8 out[4]);
/* Last AAAA-record result captured by dns_receive (1 if available, fills 16). */
int dns_last_result6(u8 out[16]);
/* Configure / read the resolver's nameserver (also loaded from
 * /etc/resolv.conf lazily on first use). */
void dns_set_server(struct ipv4_addr server);
struct ipv4_addr dns_get_server(void);
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
int tcp_is_close_wait(struct tcp_conn *conn);
int tcp_is_closed(struct tcp_conn *conn);
int tcp_send(struct tcp_conn *conn, const void *data, usize len);
int tcp_recv(struct tcp_conn *conn, void *buf, usize max_len, int flags);
int tcp_close(struct tcp_conn *conn);
int tcp_listen(u16 local_port, int backlog);
struct tcp_conn *tcp_accept(u16 local_port, struct ipv4_addr *client_ip, u16 *client_port);
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
void net_set_ip(struct ipv4_addr ip);
void net_set_gateway(struct ipv4_addr gw);
 
struct vfs_socket_state;
struct b1nix_sockaddr_un;
struct b1nix_pollfd;

// UNIX Domain Sockets (Internal)
int unix_init_state(struct vfs_socket_state *s);
void unix_free_state(struct vfs_socket_state *s);
int unix_bind(struct vfs_socket_state *s, const struct b1nix_sockaddr_un *addr);
int unix_listen(struct vfs_socket_state *s, int backlog);
int unix_connect(struct vfs_socket_state *s, const struct b1nix_sockaddr_un *addr);
int unix_accept(struct vfs_socket_state *s, struct vfs_socket_state *new_s);
isize unix_send(struct vfs_socket_state *s, const void *buf, usize len);
isize unix_recv(struct vfs_socket_state *s, void *buf, usize len);
int unix_poll(struct vfs_socket_state *s, struct b1nix_pollfd *pfd);

#endif
