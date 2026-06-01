#include <b1nix/net.h>

void net_poll(void) {}

int net_is_ready(void)
{
	return 0;
}

void net_dump_info(void) {}

struct mac_addr net_get_mac(void)
{
	return (struct mac_addr){{0, 0, 0, 0, 0, 0}};
}

struct ipv4_addr net_get_ip(void)
{
	return (struct ipv4_addr){{0, 0, 0, 0}};
}

struct ipv4_addr net_get_gateway(void)
{
	return (struct ipv4_addr){{0, 0, 0, 0}};
}

void net_set_ip(struct ipv4_addr ip)
{
	(void)ip;
}

void net_set_gateway(struct ipv4_addr gw)
{
	(void)gw;
}

void udp_send(struct ipv4_addr dst, u16 src_port, u16 dst_port, const void *payload, usize size)
{
	(void)dst;
	(void)src_port;
	(void)dst_port;
	(void)payload;
	(void)size;
}

void udp_send_net(struct ipv4_addr dst, u16 src_port_net, u16 dst_port_net, const void *payload, usize size)
{
	(void)dst;
	(void)src_port_net;
	(void)dst_port_net;
	(void)payload;
	(void)size;
}

int udp_register_handler(u16 port, udp_port_handler_t handler)
{
	(void)port;
	(void)handler;
	return -1;
}

struct tcp_conn *tcp_connect(struct ipv4_addr dst_ip, u16 dst_port)
{
	(void)dst_ip;
	(void)dst_port;
	return 0;
}

struct tcp_conn *tcp_connect_async(struct ipv4_addr dst_ip, u16 dst_port)
{
	(void)dst_ip;
	(void)dst_port;
	return 0;
}

int tcp_is_established(struct tcp_conn *conn)
{
	(void)conn;
	return 0;
}

int tcp_send(struct tcp_conn *conn, const void *data, usize len)
{
	(void)conn;
	(void)data;
	(void)len;
	return -1;
}

int tcp_recv(struct tcp_conn *conn, void *buf, usize max_len, int flags)
{
	(void)conn;
	(void)buf;
	(void)max_len;
	(void)flags;
	return -1;
}

int tcp_close(struct tcp_conn *conn)
{
	(void)conn;
	return 0;
}

void icmp_send_dest_unreachable(struct ipv4_addr dst, u8 code)
{
	(void)dst;
	(void)code;
}

void dhcp_tick(u64 now_ticks)
{
	(void)now_ticks;
}
