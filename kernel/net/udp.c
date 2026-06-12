#include <b1nix/net.h>
#include <b1nix/vfs.h>
#include <b1nix/mm.h>
#include <string.h>

struct udp_header {
	u16 src_port;
	u16 dst_port;
	u16 length;
	u16 checksum;
} __attribute__((packed));

struct udp_pseudo_header {
	u8 src[4];
	u8 dst[4];
	u8 zero;
	u8 protocol;
	u16 udp_length;
} __attribute__((packed));

struct udp_handler_entry {
	u16 port;
	udp_port_handler_t handler;
};

#define MAX_UDP_HANDLERS 8
static struct udp_handler_entry udp_handlers[MAX_UDP_HANDLERS];

static u16 bswap16(u16 value)
{
	return (u16)((value << 8) | (value >> 8));
}

static u16 udp_checksum(struct ipv4_addr src, struct ipv4_addr dst, const u8 *udp_pkt, usize udp_len)
{
	struct udp_pseudo_header pseudo;
	memcpy(pseudo.src, src.bytes, 4);
	memcpy(pseudo.dst, dst.bytes, 4);
	pseudo.zero = 0;
	pseudo.protocol = 17;
	pseudo.udp_length = bswap16((u16)udp_len);

	u32 sum = 0;
	const u8 *ph = (const u8 *)&pseudo;
	for (usize i = 0; i < sizeof(pseudo); i += 2) {
		sum += (u16)(((u16)ph[i] << 8) | ph[i + 1]);
	}
	for (usize i = 0; i + 1 < udp_len; i += 2) {
		sum += (u16)(((u16)udp_pkt[i] << 8) | udp_pkt[i + 1]);
	}
	if (udp_len & 1) {
		sum += (u16)((u16)udp_pkt[udp_len - 1] << 8);
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFFu) + (sum >> 16);
	}
	u16 out = (u16)~sum;
	return out ? out : 0xFFFFu;
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

	if (!vfs_socket_push_udp(dport_net, payload, payload_size)) {
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
	hdr->checksum = 0;

	memcpy(buffer + sizeof(struct udp_header), payload, size);
	hdr->checksum = bswap16(udp_checksum(net_get_ip(), dst, buffer, total_size));

	ipv4_send(dst, 17 /* UDP */, buffer, total_size);
	kfree(buffer);
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

	ipv6_send(dst, 17 /* UDP */, buffer, total_size);
	kfree(buffer);
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

	if (!vfs_socket_push_udp(hdr->dst_port, payload, payload_size))
		icmpv6_send_dest_unreachable(src, 4 /* port unreachable */, data,
		                             length);
}
