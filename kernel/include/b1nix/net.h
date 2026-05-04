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

// UDP
void udp_receive(struct ipv4_addr src, const void *data, usize size);
void udp_send(struct ipv4_addr dst, u16 src_port, u16 dst_port, const void *payload, usize size);

// DHCP
void dhcp_init(void);
void dhcp_receive(const void *data, usize size);

// DNS
void dns_resolve(const char *domain);
void dns_receive(const void *data, usize size);

// Network state
struct mac_addr net_get_mac(void);
struct ipv4_addr net_get_ip(void);
struct ipv4_addr net_get_gateway(void);
void net_set_ip(struct ipv4_addr ip);
void net_set_gateway(struct ipv4_addr gw);

#endif
