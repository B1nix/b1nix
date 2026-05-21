#ifndef B1NIX_NET_H
#define B1NIX_NET_H

#include <b1nix/types.h>

struct mac_addr {
	u8 bytes[6];
};

struct ipv4_addr {
	u8 bytes[4];
};

void net_init(void);
void net_poll(void);
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

// ICMP
void icmp_receive(struct ipv4_addr src, const void *data, usize size);
u32 icmp_echo_reply_count(void);

// UDP
void udp_receive(struct ipv4_addr src, const void *data, usize size);
void udp_send(struct ipv4_addr dst, u16 src_port, u16 dst_port, const void *payload, usize size);

// DHCP
void dhcp_init(void);
void dhcp_receive(const void *data, usize size);

// DNS
void dns_resolve(const char *domain);
void dns_receive(const void *data, usize size);

// TCP
void tcp_receive(struct ipv4_addr src, const void *data, usize size);
struct tcp_conn;
struct tcp_conn *tcp_connect(struct ipv4_addr dst_ip, u16 dst_port);
struct tcp_conn *tcp_connect_async(struct ipv4_addr dst_ip, u16 dst_port);
int tcp_is_established(struct tcp_conn *conn);
int tcp_send(struct tcp_conn *conn, const void *data, usize len);
int tcp_recv(struct tcp_conn *conn, void *buf, usize max_len);
int tcp_close(struct tcp_conn *conn);
int tcp_listen(u16 local_port, int backlog);
struct tcp_conn *tcp_accept(u16 local_port, struct ipv4_addr *client_ip, u16 *client_port);
int tcp_pending_connections(u16 local_port);
int tcp_network_ready(void);
void tcp_timer_tick(void);

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
