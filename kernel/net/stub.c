#include <b1nix/net.h>

void net_poll(void) {}

void udp_send(struct ipv4_addr dst, u16 src_port, u16 dst_port, const void *payload, usize size)
{
	(void)dst;
	(void)src_port;
	(void)dst_port;
	(void)payload;
	(void)size;
}

struct tcp_conn *tcp_connect(struct ipv4_addr dst_ip, u16 dst_port)
{
	(void)dst_ip;
	(void)dst_port;
	return 0;
}

int tcp_send(struct tcp_conn *conn, const void *data, usize len)
{
	(void)conn;
	(void)data;
	(void)len;
	return -1;
}

int tcp_recv(struct tcp_conn *conn, void *buf, usize max_len)
{
	(void)conn;
	(void)buf;
	(void)max_len;
	return -1;
}

int tcp_close(struct tcp_conn *conn)
{
	(void)conn;
	return 0;
}
