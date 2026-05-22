#include <b1nix/net.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <string.h>

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

struct ipv4_header {
	u8 ihl_version;
	u8 tos;
	u16 total_len;
	u16 id;
	u16 frag_offset;
	u8 ttl;
	u8 protocol;
	u16 checksum;
	struct ipv4_addr src;
	struct ipv4_addr dst;
} __attribute__((packed));

static u16 bswap16(u16 value)
{
	return (u16)((value << 8) | (value >> 8));
}

static u16 ipv4_checksum(const u8 *data, usize size)
{
	u32 sum = 0;
	for (usize i = 0; i + 1 < size; i += 2) {
		sum += ((u16)data[i] << 8) | data[i + 1];
	}
	if ((size & 1) != 0) {
		sum += (u16)data[size - 1] << 8;
	}
	while ((sum >> 16) != 0) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (u16)~sum;
}

static int ipv4_is_broadcast(struct ipv4_addr ip)
{
	return ip.bytes[0] == 255 && ip.bytes[1] == 255 &&
	       ip.bytes[2] == 255 && ip.bytes[3] == 255;
}

void ipv4_receive(const void *data, usize size)
{
	if (size < sizeof(struct ipv4_header)) return;
	const struct ipv4_header *hdr = data;

	if ((hdr->ihl_version >> 4) != 4) return;
	usize ihl = (hdr->ihl_version & 0x0F) * 4;
	if (ihl < sizeof(struct ipv4_header) || ihl > size) return;

	u16 total_len = bswap16(hdr->total_len);
	if (total_len > size || total_len < ihl) return;
	if (ipv4_checksum((const u8 *)data, ihl) != 0) return;

	u16 frag = bswap16(hdr->frag_offset);
	if ((frag & 0x3fff) != 0) return;

	struct ipv4_addr local = net_get_ip();
	if (!ipv4_is_broadcast(hdr->dst) && memcmp(hdr->dst.bytes, local.bytes, 4) != 0) {
		return;
	}

	const void *payload = (const u8 *)data + ihl;
	usize payload_size = total_len - ihl;

	if (hdr->protocol == IP_PROTO_ICMP) {
		icmp_receive(hdr->src, payload, payload_size);
	} else if (hdr->protocol == IP_PROTO_UDP) {
		udp_receive(hdr->src, payload, payload_size);
	} else if (hdr->protocol == IP_PROTO_TCP) {
		tcp_receive(hdr->src, payload, payload_size);
	}
}

static u16 ip_id_counter = 0;

void ipv4_send(struct ipv4_addr dst, u8 protocol, const void *payload, usize size)
{
	usize total_size = sizeof(struct ipv4_header) + size;
	u8 *buffer = kzalloc(total_size);
	if (!buffer) return;

	struct ipv4_header *hdr = (struct ipv4_header *)buffer;
	hdr->ihl_version = (4 << 4) | 5;
	hdr->tos = 0;
	hdr->total_len = bswap16(total_size);
	hdr->id = bswap16(ip_id_counter++);
	hdr->frag_offset = 0;
	hdr->ttl = 64;
	hdr->protocol = protocol;
	hdr->src = net_get_ip();
	hdr->dst = dst;
	hdr->checksum = 0;

	u16 csum = ipv4_checksum((const u8 *)hdr, sizeof(struct ipv4_header));
	hdr->checksum = bswap16(csum);

	memcpy(buffer + sizeof(struct ipv4_header), payload, size);

	struct mac_addr dst_mac;
	
	// Determine if broadcast or not
	if (ipv4_is_broadcast(dst)) {
		for (int i = 0; i < 6; i++) dst_mac.bytes[i] = 0xFF;
		net_send_ethernet(dst_mac, 0x0800, buffer, total_size);
		kfree(buffer);
		return;
	}

	// Wait, we need to check if target is in subnet or use gateway.
	// We'll just assume /24 mask for now to determine if local or external.
	struct ipv4_addr my_ip = net_get_ip();
	struct ipv4_addr route_ip;
	if (dst.bytes[0] == my_ip.bytes[0] && dst.bytes[1] == my_ip.bytes[1] && dst.bytes[2] == my_ip.bytes[2]) {
		route_ip = dst;
	} else {
		route_ip = net_get_gateway();
		if (route_ip.bytes[0] == 0 && route_ip.bytes[1] == 0 &&
		    route_ip.bytes[2] == 0 && route_ip.bytes[3] == 0) {
			kfree(buffer);
			return;
		}
	}

	for (int tries = 0; tries < 25; tries++) {
		if (arp_resolve(route_ip, &dst_mac)) {
			net_send_ethernet(dst_mac, 0x0800, buffer, total_size);
			kfree(buffer);
			return;
		}
		net_poll();
		scheduler_sleep_ticks(1);
	}
	kfree(buffer);
}
