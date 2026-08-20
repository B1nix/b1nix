#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/vnet.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <string.h>

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17
#define IP_PROTO_GRE  47

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

int ipv4_is_loopback(struct ipv4_addr ip)
{
	if (ip.bytes[0] == 127)
		return 1;
	/* INADDR_ANY (0.0.0.0) means "any local address" — send to self. */
	if (ip.bytes[0] == 0 && ip.bytes[1] == 0 &&
	    ip.bytes[2] == 0 && ip.bytes[3] == 0)
		return 1;
	/* Sending to our own IP is also a local delivery (loopback). */
	struct ipv4_addr my_ip = net_get_ip();
	if (memcmp(ip.bytes, my_ip.bytes, 4) == 0)
		return 1;
	return 0;
}

/* Ones'-complement sum over the IPv4 pseudo-header plus the L4 datagram. A
 * correct packet (whose checksum field is still in place) sums to zero. */
static u16 ipv4_l4_checksum(const struct ipv4_header *hdr, u8 protocol,
                            const u8 *l4, usize l4_len)
{
	u32 sum = 0;
	for (int i = 0; i < 4; i += 2)
		sum += ((u32)hdr->src.bytes[i] << 8) | hdr->src.bytes[i + 1];
	for (int i = 0; i < 4; i += 2)
		sum += ((u32)hdr->dst.bytes[i] << 8) | hdr->dst.bytes[i + 1];
	sum += protocol;
	sum += (u32)l4_len;

	for (usize i = 0; i + 1 < l4_len; i += 2)
		sum += ((u32)l4[i] << 8) | l4[i + 1];
	if ((l4_len & 1) != 0)
		sum += (u32)l4[l4_len - 1] << 8;

	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);
	return (u16)~sum;
}

/* Folded ones'-complement sum over the IPv4 pseudo-header alone, NOT
 * complemented: the partial checksum a NIC expects to find in the field when it
 * is asked to finish the sum itself. Fixed cost, independent of payload size. */
static u16 ipv4_pseudo_sum(const struct ipv4_header *hdr, usize l4_len)
{
	u32 sum = 0;
	for (int i = 0; i < 4; i += 2)
		sum += ((u32)hdr->src.bytes[i] << 8) | hdr->src.bytes[i + 1];
	for (int i = 0; i < 4; i += 2)
		sum += ((u32)hdr->dst.bytes[i] << 8) | hdr->dst.bytes[i + 1];
	sum += hdr->protocol;
	sum += (u32)l4_len;
	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);
	return (u16)sum;
}

/* Bumped from whichever CPU drained the ring, so it is incremented atomically
 * rather than read-modify-written under no lock at all. */
static u64 rx_csum_errors;

u64 ipv4_rx_csum_errors(void)
{
	return __atomic_load_n(&rx_csum_errors, __ATOMIC_RELAXED);
}

/*
 * Verify the TCP/UDP checksum of an inbound datagram.
 *
 * Until this existed the stack computed checksums on transmit and trusted
 * whatever arrived, so a corrupted segment was fed straight into the TCP state
 * machine or handed to a socket. UDP is allowed to carry a zero checksum
 * ("not computed") over IPv4; everything else must verify.
 */
static int ipv4_l4_checksum_ok(const struct ipv4_header *hdr, const u8 *l4,
                               usize l4_len)
{
	if (hdr->protocol == IP_PROTO_UDP) {
		if (l4_len < 8)
			return 0;
		if (l4[6] == 0 && l4[7] == 0)
			return 1;    /* checksum not computed by the sender */
	} else if (hdr->protocol == IP_PROTO_TCP) {
		if (l4_len < 20)
			return 0;
	} else {
		return 1;
	}
	return ipv4_l4_checksum(hdr, hdr->protocol, l4, l4_len) == 0;
}

void ipv4_receive(const void *data, usize size)
{
	ipv4_receive_flags(data, size, 0);
}

void ipv4_receive_flags(const void *data, usize size, u32 rx_flags)
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
	if (!ipv4_is_broadcast(hdr->dst) && !ipv4_is_loopback(hdr->dst) &&
	    memcmp(hdr->dst.bytes, local.bytes, 4) != 0) {
		return;
	}

	const void *payload = (const u8 *)data + ihl;
	usize payload_size = total_len - ihl;

	/* A frame the NIC already validated (GUEST_CSUM), a loopback datagram or a
	 * locally injected segment carries NET_RX_F_CSUM_OK and skips the software
	 * pass. Everything off the wire is checked here. */
	if (!(rx_flags & NET_RX_F_CSUM_OK) &&
	    !ipv4_l4_checksum_ok(hdr, payload, payload_size)) {
		__atomic_fetch_add(&rx_csum_errors, 1, __ATOMIC_RELAXED);
		return;
	}

	if (hdr->protocol == IP_PROTO_ICMP) {
		icmp_receive(hdr->src, payload, payload_size);
	} else if (hdr->protocol == IP_PROTO_UDP) {
		udp_receive(hdr->src, payload, payload_size);
	} else if (hdr->protocol == IP_PROTO_TCP) {
		tcp_receive(hdr->src, payload, payload_size);
	} else if (hdr->protocol == IP_PROTO_GRE) {
		/* An encapsulated ethernet frame for one of the gretap devices. */
		gre_input(hdr->src, payload, payload_size);
	}
}

/*
 * Fill in the TCP/UDP checksum of a datagram whose sender left the field zero
 * (IPV4_TX_F_CSUM_L4), and report the NETDEV_TX_F_* flags it must go out with.
 *
 * This is the only place that decides. It runs after the header is final, so
 * the pseudo-header uses the source address actually on the wire — including
 * the loopback rewrite, which the old per-protocol code got wrong — and it runs
 * once the egress interface is known, so a NIC that advertised
 * NETDEV_F_TX_CSUM gets a partial sum to finish (12 bytes of work here instead
 * of a pass over the whole payload) while everything else is completed in
 * software right here.
 */
static u32 ipv4_apply_l4_csum(u8 *buffer, usize total_size, u32 ip_tx_flags,
                              struct netdev *dev)
{
	if (!(ip_tx_flags & IPV4_TX_F_CSUM_L4))
		return 0;

	struct ipv4_header *hdr = (struct ipv4_header *)buffer;
	usize ihl = (usize)(hdr->ihl_version & 0x0F) * 4;
	if (ihl > total_size)
		return 0;
	u8 *l4 = buffer + ihl;
	usize l4_len = total_size - ihl;

	usize off;
	if (hdr->protocol == IP_PROTO_TCP)
		off = 16;
	else if (hdr->protocol == IP_PROTO_UDP)
		off = 6;
	else
		return 0;
	if (l4_len < off + 2)
		return 0;

	if (dev && (dev->features & NETDEV_F_TX_CSUM)) {
		u16 partial = ipv4_pseudo_sum(hdr, l4_len);
		l4[off] = (u8)(partial >> 8);
		l4[off + 1] = (u8)(partial & 0xFF);
		return NETDEV_TX_F_PARTIAL_CSUM;
	}

	l4[off] = 0;
	l4[off + 1] = 0;
	u16 csum = ipv4_l4_checksum(hdr, hdr->protocol, l4, l4_len);
	/* A zero UDP checksum means "not computed", so the all-ones form is sent
	 * instead (RFC 768). TCP has no such rule. */
	if (csum == 0 && hdr->protocol == IP_PROTO_UDP)
		csum = 0xFFFF;
	l4[off] = (u8)(csum >> 8);
	l4[off + 1] = (u8)(csum & 0xFF);
	return 0;
}

static u16 ip_id_counter = 0;

void ipv4_send(struct ipv4_addr dst, u8 protocol, const void *payload, usize size)
{
	ipv4_send_tx(dst, protocol, payload, size, 0);
}

void ipv4_send_tx(struct ipv4_addr dst, u8 protocol, const void *payload,
                  usize size, u32 ip_tx_flags)
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
	if (ipv4_is_loopback(dst)) {
		hdr->src = dst;
	}
	hdr->checksum = 0;

	u16 csum = ipv4_checksum((const u8 *)hdr, sizeof(struct ipv4_header));
	hdr->checksum = bswap16(csum);

	memcpy(buffer + sizeof(struct ipv4_header), payload, size);

	if (ipv4_is_loopback(dst)) {
		/* No device, so the checksum is completed in software here. */
		(void)ipv4_apply_l4_csum(buffer, total_size, ip_tx_flags, 0);
		/* Defer delivery instead of recursing into ipv4_receive here: a
		 * synchronous loopback path re-enters the TCP state machine mid-send
		 * and deadlocks multi-packet exchanges (SSH handshake). net_poll drains
		 * the queue in a clean context. */
		net_loopback_enqueue(buffer, total_size, 0);
		kfree(buffer);
		return;
	}

	struct mac_addr dst_mac;
	
	// Determine if broadcast or not
	if (ipv4_is_broadcast(dst)) {
		for (int i = 0; i < 6; i++) dst_mac.bytes[i] = 0xFF;
		u32 tx = ipv4_apply_l4_csum(buffer, total_size, ip_tx_flags,
		                            netdev_active());
		net_send_ethernet_tx(0, dst_mac, 0x0800, buffer, total_size, tx);
		kfree(buffer);
		return;
	}

	/* M84: consult the FIB. Longest-prefix-match picks the next hop — the
	 * destination itself for an on-link route, the gateway otherwise — and
	 * the interface to send it out of. No route means the destination is
	 * unreachable, so drop rather than guessing a /24 and firing ARP at a
	 * host that isn't on this link. */
	/* M84: hash the 5-tuple so equal-cost routes spread flows, not packets.
	 * The transport header sits at the start of the payload for TCP/UDP, which
	 * is the only case where ports exist at all. */
	u16 sport = 0, dport = 0;
	if ((protocol == IP_PROTO_TCP || protocol == IP_PROTO_UDP) && size >= 4) {
		const u8 *l4 = payload;
		sport = (u16)(((u16)l4[0] << 8) | l4[1]);
		dport = (u16)(((u16)l4[2] << 8) | l4[3]);
	}
	struct ipv4_addr local = net_get_ip();
	u32 flow = route_flow_hash(local.bytes, dst.bytes, 4, protocol, sport, dport);

	struct ipv4_addr route_ip;
	int oif = 0;
	if (!route_lookup_ex(local, dst, flow, 0, &route_ip, 0, &oif)) {
		kfree(buffer);
		return;
	}
	struct netdev *dev = oif ? netdev_by_index(oif) : 0;

	for (int tries = 0; tries < 25; tries++) {
		if (arp_resolve_dev(route_ip, &dst_mac, dev)) {
			u32 tx = ipv4_apply_l4_csum(buffer, total_size, ip_tx_flags,
			                            dev ? dev : netdev_active());
			net_send_ethernet_tx(dev, dst_mac, 0x0800, buffer, total_size, tx);
			kfree(buffer);
			return;
		}
		net_poll();
		scheduler_sleep_ticks(1);
	}
	kfree(buffer);
}
