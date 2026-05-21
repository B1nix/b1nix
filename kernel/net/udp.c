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

void udp_send(struct ipv4_addr dst, u16 src_port, u16 dst_port, const void *payload, usize size)
{
	usize total_size = sizeof(struct udp_header) + size;
	u8 *buffer = kzalloc(total_size);
	if (!buffer) return;

	struct udp_header *hdr = (struct udp_header *)buffer;
	hdr->src_port = bswap16(src_port);
	hdr->dst_port = bswap16(dst_port);
	hdr->length = bswap16(total_size);
	hdr->checksum = 0; // Optional for UDP over IPv4, so we leave it 0

	memcpy(buffer + sizeof(struct udp_header), payload, size);

	ipv4_send(dst, 17 /* UDP */, buffer, total_size);
	kfree(buffer);
}
