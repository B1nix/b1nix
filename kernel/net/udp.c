#include <b1nix/net.h>
#include <b1nix/netproto.h>
#include <b1nix/netdev.h>
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <string.h>

struct udp_header {
	u16 src_port;
	u16 dst_port;
	u16 length;
	u16 checksum;
} __attribute__((packed));

struct udp_handler_entry {
	u16 port;
	udp_port_handler_t handler;
};

#define MAX_UDP_HANDLERS 32
static struct udp_handler_entry udp_handlers[MAX_UDP_HANDLERS];

static u16 bswap16(u16 value)
{
	return (u16)((value << 8) | (value >> 8));
}

int udp_register_handler(u16 port, udp_port_handler_t handler)
{
	if (!handler) return -1;
	for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
		if (udp_handlers[i].handler && udp_handlers[i].port == port) {
			udp_handlers[i].handler = handler;
			return 0;
		}
	}
	for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
		if (!udp_handlers[i].handler) {
			udp_handlers[i].port = port;
			udp_handlers[i].handler = handler;
			return 0;
		}
	}
	return -1;
}

void udp_unregister_handler(u16 port)
{
	for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
		if (udp_handlers[i].handler && udp_handlers[i].port == port) {
			udp_handlers[i].handler = 0;
			udp_handlers[i].port = 0;
			return;
		}
	}
}

void udp_receive(struct ipv4_addr src, const void *data, usize size)
{
	if (size < sizeof(struct udp_header)) return;
	const struct udp_header *hdr = data;

	u16 length = bswap16(hdr->length);
	if (length > size || length < sizeof(struct udp_header)) return;

	const void *payload = (const u8 *)data + sizeof(struct udp_header);
	usize payload_size = length - sizeof(struct udp_header);

	u16 dport = bswap16(hdr->dst_port);
	u16 dport_net = hdr->dst_port;

	if (!vfs_socket_push_udp(dport_net, payload, payload_size, src.bytes, 0,
	                         hdr->src_port)) {
		for (int i = 0; i < MAX_UDP_HANDLERS; i++) {
			if (udp_handlers[i].handler && udp_handlers[i].port == dport) {
				udp_handlers[i].handler(payload, payload_size);
				return;
			}
		}
		/* Port unreachable */
		icmp_send_dest_unreachable(src, 3);
	}
}

void udp_send_net(struct ipv4_addr dst, u16 src_port_net, u16 dst_port_net, const void *payload, usize size)
{
	usize total_size = sizeof(struct udp_header) + size;
	u8 *buffer = kzalloc(total_size);
	if (!buffer) return;

	struct udp_header *hdr = (struct udp_header *)buffer;
	hdr->src_port = src_port_net;
	hdr->dst_port = dst_port_net;
	hdr->length = bswap16(total_size);
	/* Left zero on purpose: the IP layer fills the checksum in, because only it
	 * knows the source address that ends up in the header and which interface
	 * (if any) can finish the sum in hardware. */
	hdr->checksum = 0;

	memcpy(buffer + sizeof(struct udp_header), payload, size);

	int is_loopback = ipv4_is_loopback(dst);
	ipv4_send_tx(dst, 17 /* UDP */, buffer, total_size, IPV4_TX_F_CSUM_L4);
	kfree(buffer);
	/* For loopback the packet was enqueued (not delivered synchronously) to
	 * avoid re-entering TCP state machines.  UDP is stateless, so we can
	 * drain the loopback queue immediately: this lets recv() see the packet
	 * right away (required for sendto-self + recv ordering tests). */
	if (is_loopback)
		net_loopback_drain();
}

void udp_send(struct ipv4_addr dst, u16 src_port, u16 dst_port, const void *payload, usize size)
{
	udp_send_net(dst, bswap16(src_port), bswap16(dst_port), payload, size);
}

/* UDP-over-IPv6 checksum: ones'-complement sum over the IPv6 pseudo-header
 * (src, dst, 32-bit UDP length, next header 17) and the UDP datagram. */
static u16 udp6_checksum(struct in6_addr_k src, struct in6_addr_k dst,
                         const u8 *udp_pkt, usize udp_len)
{
	u32 sum = 0;
	for (int i = 0; i < 16; i += 2)
		sum += ((u16)src.bytes[i] << 8) | src.bytes[i + 1];
	for (int i = 0; i < 16; i += 2)
		sum += ((u16)dst.bytes[i] << 8) | dst.bytes[i + 1];
	sum += (u16)(udp_len >> 16);
	sum += (u16)(udp_len & 0xffff);
	sum += 17; /* next header = UDP */
	for (usize i = 0; i + 1 < udp_len; i += 2)
		sum += ((u16)udp_pkt[i] << 8) | udp_pkt[i + 1];
	if (udp_len & 1)
		sum += (u16)udp_pkt[udp_len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	u16 out = (u16)~sum;
	return out ? out : 0xFFFFu;
}

void udp6_send(struct in6_addr_k dst, u16 src_port_net, u16 dst_port_net,
               const void *payload, usize size)
{
	usize total_size = sizeof(struct udp_header) + size;
	u8 *buffer = kzalloc(total_size);
	if (!buffer) return;

	struct udp_header *hdr = (struct udp_header *)buffer;
	hdr->src_port = src_port_net;
	hdr->dst_port = dst_port_net;
	hdr->length = bswap16((u16)total_size);
	hdr->checksum = 0;

	memcpy(buffer + sizeof(struct udp_header), payload, size);
	/* On the ::1 loopback path the source address equals the destination. */
	hdr->checksum = bswap16(udp6_checksum(dst, dst, buffer, total_size));

	net_proto_ipv6_send(dst, 17 /* UDP */, buffer, total_size);
	kfree(buffer);
}

/* M84: in-kernel consumers of well-known IPv6 UDP ports (DHCPv6 is the first
 * one). Mirrors the IPv4 handler table, but the callback also gets the source
 * address — a DHCPv6 client needs to know which server answered. */
#define UDP6_MAX_HANDLERS 4
static struct {
	u16 port;
	udp6_port_handler_t handler;
} udp6_handlers[UDP6_MAX_HANDLERS];
static usize udp6_handler_count;

int udp6_register_handler(u16 port, udp6_port_handler_t handler)
{
	for (usize i = 0; i < udp6_handler_count; i++) {
		if (udp6_handlers[i].port == port) {
			udp6_handlers[i].handler = handler;
			return 0;
		}
	}
	if (udp6_handler_count >= UDP6_MAX_HANDLERS)
		return -1;
	udp6_handlers[udp6_handler_count].port = port;
	udp6_handlers[udp6_handler_count].handler = handler;
	udp6_handler_count++;
	return 0;
}

void udp6_receive(struct in6_addr_k src, struct in6_addr_k dst,
                  const void *data, usize size)
{
	(void)dst;
	if (size < sizeof(struct udp_header)) return;
	const struct udp_header *hdr = data;

	u16 length = bswap16(hdr->length);
	if (length > size || length < sizeof(struct udp_header)) return;

	const void *payload = (const u8 *)data + sizeof(struct udp_header);
	usize payload_size = length - sizeof(struct udp_header);

	u16 dport = bswap16(hdr->dst_port);
	for (usize i = 0; i < udp6_handler_count; i++) {
		if (udp6_handlers[i].port == dport && udp6_handlers[i].handler) {
			udp6_handlers[i].handler(src, payload, payload_size);
			return;
		}
	}

	if (!vfs_socket_push_udp(hdr->dst_port, payload, payload_size, src.bytes, 1,
	                         hdr->src_port))
		net_proto_icmp6_unreach(src, 4 /* port unreachable */, data, length);
}
