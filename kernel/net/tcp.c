#include <b1nix/net.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <string.h>

/* TCP protocol number in IPv4 */
#define IP_PROTO_TCP 6

/* TCP flags */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* TCP states */
#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RECEIVED 3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT1    5
#define TCP_FIN_WAIT2    6
#define TCP_CLOSE_WAIT   7
#define TCP_CLOSING      8
#define TCP_LAST_ACK     9
#define TCP_TIME_WAIT    10

struct tcp_header {
	u16 src_port;
	u16 dst_port;
	u32 seq_num;
	u32 ack_num;
	u8  data_offset;  /* upper 4 bits = header length in 32-bit words */
	u8  flags;
	u16 window;
	u16 checksum;
	u16 urgent;
} __attribute__((packed));

/* TCP pseudo-header for checksum calculation */
struct tcp_pseudo {
	struct ipv4_addr src;
	struct ipv4_addr dst;
	u8  zero;
	u8  protocol;
	u16 tcp_length;
} __attribute__((packed));

#define MAX_TCP_CONNS 4
#define TCP_RECV_BUF_SIZE 4096
#define TCP_SEND_BUF_SIZE 4096
#define TCP_MSS 1460

struct tcp_conn {
	int used;
	int state;
	struct ipv4_addr remote_ip;
	u16 remote_port;
	u16 local_port;
	u32 snd_una;    /* oldest unacked sequence number */
	u32 snd_nxt;    /* next sequence number to send */
	u32 rcv_nxt;    /* next expected receive sequence number */
	u32 iss;        /* initial send sequence number */
	u32 irs;        /* initial receive sequence number */
	u8  recv_buf[TCP_RECV_BUF_SIZE];
	u32 recv_len;
	u32 recv_read;
};

static struct tcp_conn tcp_conns[MAX_TCP_CONNS];
static u16 next_local_port = 1025;
static u32 tcp_iss_counter = 0;

static u16 bswap16(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static u32 bswap32(u32 v)
{
	return (u32)(((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
	             ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF));
}

static u16 tcp_checksum(struct ipv4_addr src, struct ipv4_addr dst,
                        const void *tcp_data, usize tcp_len)
{
	struct tcp_pseudo pseudo;
	pseudo.src = src;
	pseudo.dst = dst;
	pseudo.zero = 0;
	pseudo.protocol = 6; /* TCP */
	pseudo.tcp_length = bswap16((u16)tcp_len);

	u32 sum = 0;
	const u8 *p = (const u8 *)&pseudo;
	for (usize i = 0; i + 1 < sizeof(pseudo); i += 2)
		sum += ((u16)p[i] << 8) | p[i + 1];

	p = (const u8 *)tcp_data;
	for (usize i = 0; i + 1 < tcp_len; i += 2)
		sum += ((u16)p[i] << 8) | p[i + 1];

	if ((tcp_len & 1) != 0)
		sum += (u16)p[tcp_len - 1] << 8;

	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);

	return (u16)~sum;
}

/* ── Allocate local port ── */
static u16 tcp_alloc_port(void)
{
	u16 port = next_local_port++;
	if (next_local_port < 1025) next_local_port = 1025;
	return port;
}

/* ── Find connection by remote ── */
static struct tcp_conn *tcp_find_conn(struct ipv4_addr remote_ip, u16 remote_port, u16 local_port)
{
	for (int i = 0; i < MAX_TCP_CONNS; i++) {
		if (!tcp_conns[i].used) continue;
		if (tcp_conns[i].remote_port == remote_port &&
		    tcp_conns[i].local_port == local_port &&
		    memcmp(&tcp_conns[i].remote_ip, &remote_ip, 4) == 0) {
			return &tcp_conns[i];
		}
	}
	return 0;
}

/* ── Create new TCP connection (active open) ── */
struct tcp_conn *tcp_connect(struct ipv4_addr dst_ip, u16 dst_port)
{
	/* Find free slot */
	struct tcp_conn *conn = 0;
	for (int i = 0; i < MAX_TCP_CONNS; i++) {
		if (!tcp_conns[i].used) {
			conn = &tcp_conns[i];
			break;
		}
	}
	if (!conn) {
		console_write("tcp: no free connection slots\n");
		return 0;
	}

	memset(conn, 0, sizeof(*conn));
	conn->used = 1;
	conn->state = TCP_CLOSED;
	conn->remote_ip = dst_ip;
	conn->remote_port = dst_port;
	conn->local_port = tcp_alloc_port();

	/* Generate initial sequence number */
	tcp_iss_counter += 1000;
	conn->iss = tcp_iss_counter;
	conn->snd_una = conn->iss;
	conn->snd_nxt = conn->iss;

	/* Send SYN */
	u8 packet[sizeof(struct tcp_header)];
	memset(packet, 0, sizeof(packet));
	struct tcp_header *tcp = (struct tcp_header *)packet;
	tcp->src_port = bswap16(conn->local_port);
	tcp->dst_port = bswap16(dst_port);
	tcp->seq_num = bswap32(conn->iss);
	tcp->ack_num = 0;
	tcp->data_offset = (5 << 4);
	tcp->flags = TCP_SYN;
	tcp->window = bswap16(TCP_RECV_BUF_SIZE);
	tcp->checksum = tcp_checksum(net_get_ip(), dst_ip, packet, sizeof(packet));

	conn->state = TCP_SYN_SENT;

	ipv4_send(dst_ip, IP_PROTO_TCP, packet, sizeof(packet));

	/* Wait for SYN-ACK (poll a few times) */
	for (int tries = 0; tries < 200 && conn->state == TCP_SYN_SENT; tries++) {
		net_poll();
	}

	if (conn->state != TCP_ESTABLISHED) {
		console_write("tcp: connect failed\n");
		conn->used = 0;
		return 0;
	}

	console_write("tcp: connected\n");
	return conn;
}

/* ── TCP send data ── */
int tcp_send(struct tcp_conn *conn, const void *data, usize len)
{
	if (!conn || conn->state != TCP_ESTABLISHED) return -1;
	if (len == 0) return 0;

	usize to_send = len;
	if (to_send > TCP_MSS) to_send = TCP_MSS;

	usize packet_len = sizeof(struct tcp_header) + to_send;
	u8 *packet = kzalloc(packet_len);
	if (!packet) return -1;

	struct tcp_header *tcp = (struct tcp_header *)packet;
	tcp->src_port = bswap16(conn->local_port);
	tcp->dst_port = bswap16(conn->remote_port);
	tcp->seq_num = bswap32(conn->snd_nxt);
	tcp->ack_num = bswap32(conn->rcv_nxt);
	tcp->data_offset = (5 << 4);
	tcp->flags = TCP_PSH | TCP_ACK;
	tcp->window = bswap16(TCP_RECV_BUF_SIZE - conn->recv_len);

	memcpy(packet + sizeof(struct tcp_header), data, to_send);

	tcp->checksum = tcp_checksum(net_get_ip(), conn->remote_ip, packet, packet_len);

	conn->snd_nxt += (u32)to_send;

	ipv4_send(conn->remote_ip, IP_PROTO_TCP, packet, packet_len);
	kfree(packet);

	return (int)to_send;
}

/* ── TCP receive data (non-blocking) ── */
int tcp_recv(struct tcp_conn *conn, void *buf, usize max_len)
{
	if (!conn) return -1;
	if (conn->recv_read >= conn->recv_len) {
		/* Poll for new data */
		net_poll();
	}
	if (conn->recv_read >= conn->recv_len) return 0;

	usize avail = conn->recv_len - conn->recv_read;
	if (avail > max_len) avail = max_len;

	memcpy(buf, conn->recv_buf + conn->recv_read, avail);
	conn->recv_read += (u32)avail;

	/* Compact buffer if all read */
	if (conn->recv_read >= conn->recv_len) {
		conn->recv_len = 0;
		conn->recv_read = 0;
	}

	return (int)avail;
}

/* ── TCP close ── */
int tcp_close(struct tcp_conn *conn)
{
	if (!conn || !conn->used) return -1;

	/* Send FIN */
	u8 packet[sizeof(struct tcp_header)];
	memset(packet, 0, sizeof(packet));
	struct tcp_header *tcp = (struct tcp_header *)packet;
	tcp->src_port = bswap16(conn->local_port);
	tcp->dst_port = bswap16(conn->remote_port);
	tcp->seq_num = bswap32(conn->snd_nxt);
	tcp->ack_num = bswap32(conn->rcv_nxt);
	tcp->data_offset = (5 << 4);
	tcp->flags = TCP_FIN | TCP_ACK;
	tcp->window = bswap16(TCP_RECV_BUF_SIZE);
	tcp->checksum = tcp_checksum(net_get_ip(), conn->remote_ip, packet, sizeof(packet));

	conn->state = TCP_FIN_WAIT1;
	conn->snd_nxt++;

	ipv4_send(conn->remote_ip, IP_PROTO_TCP, packet, sizeof(packet));

	/* Wait for FIN-ACK (poll a bit) */
	for (int tries = 0; tries < 50 && conn->state != TCP_CLOSED; tries++) {
		net_poll();
	}

	conn->used = 0;
	return 0;
}

/* ── Receive TCP segment from IP layer ── */
void tcp_receive(struct ipv4_addr src, const void *data, usize size)
{
	if (size < sizeof(struct tcp_header)) return;

	const struct tcp_header *tcp = (const struct tcp_header *)data;
	u16 src_port = bswap16(tcp->src_port);
	u16 dst_port = bswap16(tcp->dst_port);
	u8  flags   = tcp->flags;
	u32 seq     = bswap32(tcp->seq_num);
	u32 ack     = bswap32(tcp->ack_num);

	usize data_offset = (tcp->data_offset >> 4) * 4;
	if (data_offset < sizeof(struct tcp_header)) return;

	const u8 *payload = (const u8 *)data + data_offset;
	usize payload_size = (size > data_offset) ? size - data_offset : 0;

	/* Find connection */
	struct tcp_conn *conn = tcp_find_conn(src, src_port, dst_port);

	if (!conn) {
		/* No connection — send RST unless it's a RST itself */
		if (!(flags & TCP_RST)) {
			u8 rst[sizeof(struct tcp_header)];
			memset(rst, 0, sizeof(rst));
			struct tcp_header *r = (struct tcp_header *)rst;
			r->src_port = tcp->dst_port;
			r->dst_port = tcp->src_port;
			r->seq_num = 0;
			r->ack_num = (flags & TCP_ACK) ? ack : bswap32(seq + (u32)payload_size + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0));
			r->data_offset = (5 << 4);
			r->flags = TCP_RST | (flags & TCP_ACK ? TCP_ACK : 0);
			r->window = 0;
			r->checksum = tcp_checksum(net_get_ip(), src, rst, sizeof(rst));
			ipv4_send(src, IP_PROTO_TCP, rst, sizeof(rst));
		}
		return;
	}

	switch (conn->state) {
	case TCP_SYN_SENT:
		/* Expect SYN-ACK */
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
			if (ack == conn->snd_nxt + 1 || ack == conn->snd_una + 1 || ack >= conn->iss) {
				conn->irs = seq;
				conn->rcv_nxt = seq + 1;
				conn->snd_una = ack;
				conn->snd_nxt = ack;

				/* Send ACK */
				u8 ack_pkt[sizeof(struct tcp_header)];
				memset(ack_pkt, 0, sizeof(ack_pkt));
				struct tcp_header *a = (struct tcp_header *)ack_pkt;
				a->src_port = bswap16(conn->local_port);
				a->dst_port = bswap16(conn->remote_port);
				a->seq_num = bswap32(conn->snd_nxt);
				a->ack_num = bswap32(conn->rcv_nxt);
				a->data_offset = (5 << 4);
				a->flags = TCP_ACK;
				a->window = bswap16(TCP_RECV_BUF_SIZE);
				a->checksum = tcp_checksum(net_get_ip(), src, ack_pkt, sizeof(ack_pkt));
				ipv4_send(src, IP_PROTO_TCP, ack_pkt, sizeof(ack_pkt));

				conn->state = TCP_ESTABLISHED;
			}
		} else if (flags & TCP_RST) {
			conn->state = TCP_CLOSED;
		}
		break;

	case TCP_ESTABLISHED:
	case TCP_FIN_WAIT1:
	case TCP_FIN_WAIT2:
		/* Check ACK */
		if (flags & TCP_ACK) {
			if (ack > conn->snd_una || ack == conn->snd_una) {
				conn->snd_una = ack;
			}
		}

		/* Receive data */
		if (payload_size > 0 && (flags & (TCP_PSH | TCP_ACK))) {
			if (seq == conn->rcv_nxt) {
				u32 space = TCP_RECV_BUF_SIZE - conn->recv_len;
				if (payload_size > space) payload_size = space;
				memcpy(conn->recv_buf + conn->recv_len, payload, payload_size);
				conn->recv_len += (u32)payload_size;
				conn->rcv_nxt += (u32)payload_size;

				/* Send ACK for data */
				u8 ack_pkt[sizeof(struct tcp_header)];
				memset(ack_pkt, 0, sizeof(ack_pkt));
				struct tcp_header *a = (struct tcp_header *)ack_pkt;
				a->src_port = bswap16(conn->local_port);
				a->dst_port = bswap16(conn->remote_port);
				a->seq_num = bswap32(conn->snd_nxt);
				a->ack_num = bswap32(conn->rcv_nxt);
				a->data_offset = (5 << 4);
				a->flags = TCP_ACK;
				a->window = bswap16(TCP_RECV_BUF_SIZE - conn->recv_len);
				a->checksum = tcp_checksum(net_get_ip(), src, ack_pkt, sizeof(ack_pkt));
				ipv4_send(src, IP_PROTO_TCP, ack_pkt, sizeof(ack_pkt));
			}
		}

		/* Handle FIN */
		if (flags & TCP_FIN) {
			conn->rcv_nxt++;

			/* Send ACK for FIN */
			u8 ack_pkt[sizeof(struct tcp_header)];
			memset(ack_pkt, 0, sizeof(ack_pkt));
			struct tcp_header *a = (struct tcp_header *)ack_pkt;
			a->src_port = bswap16(conn->local_port);
			a->dst_port = bswap16(conn->remote_port);
			a->seq_num = bswap32(conn->snd_nxt);
			a->ack_num = bswap32(conn->rcv_nxt);
			a->data_offset = (5 << 4);
			a->flags = TCP_ACK;
			a->window = 0;
			a->checksum = tcp_checksum(net_get_ip(), src, ack_pkt, sizeof(ack_pkt));
			ipv4_send(src, IP_PROTO_TCP, ack_pkt, sizeof(ack_pkt));

			if (conn->state == TCP_ESTABLISHED) {
				conn->state = TCP_CLOSE_WAIT;
			} else if (conn->state == TCP_FIN_WAIT1) {
				conn->state = TCP_CLOSED;
			} else if (conn->state == TCP_FIN_WAIT2) {
				conn->state = TCP_CLOSED;
			}
		}

		/* Handle RST */
		if (flags & TCP_RST) {
			conn->state = TCP_CLOSED;
		}
		break;

	case TCP_CLOSE_WAIT:
		/* Already received FIN from peer, waiting for app to close */
		if (flags & TCP_ACK) conn->snd_una = ack;
		break;

	case TCP_LAST_ACK:
		if (flags & TCP_ACK) {
			conn->state = TCP_CLOSED;
		}
		break;

	default:
		break;
	}
}

/* ── Check if network is available ── */
int tcp_network_ready(void)
{
	struct ipv4_addr ip = net_get_ip();
	return (ip.bytes[0] != 0 || ip.bytes[1] != 0 || ip.bytes[2] != 0 || ip.bytes[3] != 0);
}
