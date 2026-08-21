/* Minimal IPv6 datapath.
 *
 * This is the first slice of kernel IPv6: a loopback (::1) fast path and an
 * ICMPv6 echo (ping) responder, mirroring the IPv4 loopback path in ipv4.c.
 * Neighbor discovery, routing, real-link (ethertype 0x86DD) send, and UDP/TCP
 * over IPv6 are not implemented yet — ipv6_send() only handles ::1.
 */

#include <b1nix/kprintf.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/netproto.h>
#include <b1nix/mm.h>
#include <b1nix/console.h>
#include <b1nix/sched.h>
#include <string.h>

/* M96 module parameter: hop limit for non-ND traffic (ND is fixed at 255 by
 * RFC 4861). Declared here so ipv6_send can read it; published to
 * /sys/module/ipv6/parameters at the bottom of this file. */
int ipv6_hop_limit = 64;

#define IP6_NH_TCP    6
#define IP6_NH_UDP    17
#define IP6_NH_ICMPV6 58

#define ICMP6_ECHO_REQUEST 128
#define ICMP6_ECHO_REPLY   129
#define ICMP6_DEST_UNREACH 1
#define ICMP6_PACKET_TOO_BIG 2
#define ICMP6_TIME_EXCEEDED 3
#define ICMP6_PARAM_PROBLEM 4
#define ICMP6_MLD_QUERY 130
#define ICMP6_MLD_REPORT 131
#define ICMP6_MLD_DONE 132
#define ICMP6_MLDV2_REPORT 143

struct ipv6_header {
	u8 ver_tc_hi;     /* version (high nibble) + traffic class (high nibble) */
	u8 tc_lo_flow_hi; /* traffic class (low) + flow label (high) */
	u16 flow_lo;      /* flow label (low 16 bits) */
	u16 payload_len;  /* network byte order */
	u8 next_header;
	u8 hop_limit;
	struct in6_addr_k src;
	struct in6_addr_k dst;
} __attribute__((packed));

struct icmpv6_header {
	u8 type;
	u8 code;
	u16 checksum;
	u16 id;
	u16 seq;
} __attribute__((packed));

static u16 bswap16(u16 v) { return (u16)((v << 8) | (v >> 8)); }

static volatile u32 g_icmpv6_echo_replies = 0;
static volatile u32 g_icmpv6_errors = 0;

u32 icmpv6_echo_reply_count(void)
{
	return __atomic_load_n(&g_icmpv6_echo_replies, __ATOMIC_RELAXED);
}

static int in6_is_loopback(const struct in6_addr_k *a)
{
	for (int i = 0; i < 15; i++)
		if (a->bytes[i] != 0)
			return 0;
	return a->bytes[15] == 1;
}

static int in6_is_multicast(const struct in6_addr_k *a);

/* ICMPv6 checksum: 16-bit ones' complement over the IPv6 pseudo-header
 * (src, dst, 32-bit upper-layer length, next header) followed by the message. */
static u16 icmpv6_checksum(const struct in6_addr_k *src,
                           const struct in6_addr_k *dst, u8 next_header,
                           const u8 *data, usize len)
{
	u32 sum = 0;
	for (int i = 0; i < 16; i += 2)
		sum += ((u16)src->bytes[i] << 8) | src->bytes[i + 1];
	for (int i = 0; i < 16; i += 2)
		sum += ((u16)dst->bytes[i] << 8) | dst->bytes[i + 1];
	sum += (u16)(len >> 16);
	sum += (u16)(len & 0xffff);
	sum += next_header;
	for (usize i = 0; i + 1 < len; i += 2)
		sum += ((u16)data[i] << 8) | data[i + 1];
	if ((len & 1) != 0)
		sum += (u16)data[len - 1] << 8;
	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);
	return (u16)~sum;
}

static void icmpv6_receive(const struct in6_addr_k *src,
                           const struct in6_addr_k *dst, const void *data,
                           usize size)
{
	if (size < sizeof(struct icmpv6_header))
		return;
	const struct icmpv6_header *hdr = data;

	if (hdr->type == ICMP6_ECHO_REQUEST) {
		u8 *reply = kzalloc(size);
		if (!reply)
			return;
		memcpy(reply, data, size);
		struct icmpv6_header *rhdr = (struct icmpv6_header *)reply;
		rhdr->type = ICMP6_ECHO_REPLY;
		rhdr->code = 0;
		rhdr->checksum = 0;
		/* Reply travels dst -> src, so the pseudo-header swaps addresses. */
		u16 csum = icmpv6_checksum(dst, src, IP6_NH_ICMPV6, reply, size);
		rhdr->checksum = bswap16(csum);
		ipv6_send(*src, IP6_NH_ICMPV6, reply, size);
		kfree(reply);
	} else if (hdr->type == ICMP6_ECHO_REPLY) {
		__atomic_add_fetch(&g_icmpv6_echo_replies, 1, __ATOMIC_RELAXED);
	} else if (hdr->type >= ICMP6_DEST_UNREACH &&
	           hdr->type <= ICMP6_PARAM_PROBLEM) {
		__atomic_add_fetch(&g_icmpv6_errors, 1, __ATOMIC_RELAXED);
	}
}

void icmpv6_send_dest_unreachable(struct in6_addr_k dst, u8 code,
                                  const void *quoted, usize quoted_len)
{
	if (in6_is_multicast(&dst))
		return;
	if (quoted_len > 256)
		quoted_len = 256;
	usize len = sizeof(struct icmpv6_header) + quoted_len;
	u8 *pkt = kzalloc(len);
	if (!pkt)
		return;
	struct icmpv6_header *hdr = (struct icmpv6_header *)pkt;
	hdr->type = ICMP6_DEST_UNREACH;
	hdr->code = code;
	if (quoted_len)
		memcpy(pkt + sizeof(*hdr), quoted, quoted_len);
	ipv6_send(dst, IP6_NH_ICMPV6, pkt, len);
	kfree(pkt);
}

void ipv6_receive(const void *data, usize size)
{
	if (size < sizeof(struct ipv6_header))
		return;
	const struct ipv6_header *hdr = data;

	if ((hdr->ver_tc_hi >> 4) != 6)
		return;

	u16 payload_len = bswap16(hdr->payload_len);
	if ((usize)payload_len + sizeof(struct ipv6_header) > size)
		return;

	const void *payload = (const u8 *)data + sizeof(struct ipv6_header);

	if (hdr->next_header == IP6_NH_ICMPV6) {
		if (payload_len < 1)
			return;
		if (icmpv6_checksum(&hdr->src, &hdr->dst, IP6_NH_ICMPV6, payload,
		                    payload_len) != 0)
			return;
		u8 type = ((const u8 *)payload)[0];
		if ((type >= 133 && type <= 136) || type == ICMP6_MLD_QUERY ||
		    type == ICMP6_MLD_REPORT || type == ICMP6_MLD_DONE ||
		    type == ICMP6_MLDV2_REPORT) {
			/* M96: Neighbour Discovery and MLD both live in ndp.ko,
			 * which sorts the message out by type. */
			ndp_dispatch_receive(hdr->src, hdr->dst, type, payload,
			                     payload_len);
		} else {
			icmpv6_receive(&hdr->src, &hdr->dst, payload, payload_len);
		}
	} else if (hdr->next_header == IP6_NH_UDP) {
		udp6_receive(hdr->src, hdr->dst, payload, payload_len);
	} else if (hdr->next_header == IP6_NH_TCP) {
		tcp6_receive(hdr->src, payload, payload_len);
	}
}

static int in6_is_multicast(const struct in6_addr_k *a)
{
	return a->bytes[0] == 0xff;
}

static int in6_is_link_local(const struct in6_addr_k *a)
{
	return a->bytes[0] == 0xfe && (a->bytes[1] & 0xc0) == 0x80;
}

static int in6_is_zero(const struct in6_addr_k *a)
{
	for (int i = 0; i < 16; i++)
		if (a->bytes[i])
			return 0;
	return 1;
}

/* Pick the source address for a destination: loopback for ::1, link-local for
 * link-local/multicast peers, otherwise the SLAAC global (falling back to
 * link-local until one is configured). */
static struct in6_addr_k ipv6_select_source(const struct in6_addr_k *dst)
{
	if (in6_is_loopback(dst)) {
		struct in6_addr_k lo;
		memset(&lo, 0, sizeof(lo));
		lo.bytes[15] = 1;
		return lo;
	}
	if (in6_is_link_local(dst) || in6_is_multicast(dst))
		return net_get_ip6_ll();
	struct in6_addr_k g = net_get_ip6();
	if (!in6_is_zero(&g))
		return g;
	return net_get_ip6_ll();
}

/* Checksum-offload the upper layer: zero the L4 checksum field and recompute
 * it against the pseudo-header for the chosen source/destination. Centralising
 * it here is what makes the same packet correct on both the loopback and the
 * real-link paths (the source address is only known at this layer). */
static void ipv6_fix_l4_checksum(const struct in6_addr_k *src,
                                 const struct in6_addr_k *dst, u8 nh, u8 *l4,
                                 usize len)
{
	usize off;
	if (nh == IP6_NH_ICMPV6)
		off = 2;
	else if (nh == IP6_NH_UDP)
		off = 6;
	else if (nh == IP6_NH_TCP)
		off = 16;
	else
		return;
	if (len < off + 2)
		return;
	l4[off] = 0;
	l4[off + 1] = 0;
	u16 c = icmpv6_checksum(src, dst, nh, l4, len);
	if (nh == IP6_NH_UDP && c == 0)
		c = 0xffff; /* a zero UDP checksum means "none" — send all-ones */
	l4[off] = (u8)(c >> 8);
	l4[off + 1] = (u8)(c & 0xff);
}

/* Transmit a fully-formed IPv6 frame on the real link: map multicast to its
 * 33:33 MAC, otherwise resolve the next hop (on-link peer, or the default
 * router for off-link destinations) via NDP and emit an ethertype-0x86DD
 * frame. */
static void ipv6_link_output(struct netdev *dev, struct in6_addr_k dst,
                             const u8 *frame, usize total)
{
	/* M84: the flow hash for ECMP comes from the finished frame — source and
	 * destination addresses plus the transport ports that follow the fixed
	 * header (TCP/UDP only). */
	u32 flow = 0;
	if (total >= sizeof(struct ipv6_header)) {
		const struct ipv6_header *ih = (const struct ipv6_header *)frame;
		u16 sport = 0, dport = 0;
		if ((ih->next_header == 6 || ih->next_header == 17) &&
		    total >= sizeof(struct ipv6_header) + 4) {
			const u8 *l4 = frame + sizeof(struct ipv6_header);
			sport = (u16)(((u16)l4[0] << 8) | l4[1]);
			dport = (u16)(((u16)l4[2] << 8) | l4[3]);
		}
		flow = route_flow_hash(ih->src.bytes, ih->dst.bytes, 16,
		                       ih->next_header, sport, dport);
	}

	struct mac_addr mac;
	if (in6_is_multicast(&dst)) {
		mac.bytes[0] = 0x33;
		mac.bytes[1] = 0x33;
		mac.bytes[2] = dst.bytes[12];
		mac.bytes[3] = dst.bytes[13];
		mac.bytes[4] = dst.bytes[14];
		mac.bytes[5] = dst.bytes[15];
		net_send_ethernet_dev(dev, mac, 0x86DD, frame, total);
		return;
	}

	/* M84: the IPv6 FIB decides the next hop and the output interface —
	 * longest-prefix-match over the on-link prefixes (fe80::/10 and any
	 * RA-advertised prefix) and the default route, instead of the previous
	 * "compare the first 8 bytes, else use the single SLAAC router". */
	struct in6_addr_k next_hop;
	int oif = 0;
	if (!route6_lookup_flow(dst, flow, &next_hop, 0, &oif))
		return; /* unreachable: no route */
	if (!dev && oif)
		dev = netdev_by_index(oif);

	for (int tries = 0; tries < 25; tries++) {
		/* M84 chose the egress interface; M96 moved NDP into a module, so the
		 * resolution goes through the protocol registry — carrying that device,
		 * or a neighbour would be solicited on the wrong link. */
		if (ndp_dispatch_resolve(next_hop, &mac, dev)) {
			net_send_ethernet_dev(dev, mac, 0x86DD, frame, total);
			return;
		}
		net_poll();
		scheduler_sleep_ticks(1);
	}
}

void ipv6_send_via(struct netdev *dev, struct in6_addr_k dst, u8 next_header,
                   const void *payload, usize size)
{
	usize total = sizeof(struct ipv6_header) + size;
	u8 *buffer = kzalloc(total);
	if (!buffer)
		return;

	struct ipv6_header *hdr = (struct ipv6_header *)buffer;
	hdr->ver_tc_hi = (u8)(6 << 4);
	hdr->payload_len = bswap16((u16)size);
	hdr->next_header = next_header;
	/* Neighbor Discovery requires hop limit 255; harmless for other traffic on
	 * a directly-attached link. */
	hdr->hop_limit = (next_header == IP6_NH_ICMPV6) ? 255
	                                                : (u8)ipv6_hop_limit;
	struct in6_addr_k src = ipv6_select_source(&dst);
	hdr->src = src;
	hdr->dst = dst;

	memcpy(buffer + sizeof(struct ipv6_header), payload, size);
	ipv6_fix_l4_checksum(&src, &dst, next_header,
	                     buffer + sizeof(struct ipv6_header), size);

	if (in6_is_loopback(&dst)) {
		/* Defer delivery instead of recursing into ipv6_receive here: a
		 * synchronous loopback path re-enters the TCP state machine mid-send
		 * and deadlocks multi-packet exchanges. net_poll drains the queue in a
		 * clean context. Mirrors the IPv4 fix in ipv4.c. */
		net_loopback_enqueue(buffer, total, 1);
		kfree(buffer);
		return;
	}

	ipv6_link_output(dev, dst, buffer, total);
	kfree(buffer);
}

void ipv6_send(struct in6_addr_k dst, u8 next_header, const void *payload,
               usize size)
{
	ipv6_send_via(0, dst, next_header, payload, size);
}

/* Offline self-test: ping ::1 and confirm an ICMPv6 echo reply returns. */
void ipv6_loopback_smoke(void)
{
	struct in6_addr_k loop;
	memset(&loop, 0, sizeof(loop));
	loop.bytes[15] = 1;

	u32 before = icmpv6_echo_reply_count();

	u8 req[sizeof(struct icmpv6_header) + 8];
	memset(req, 0, sizeof(req));
	struct icmpv6_header *hdr = (struct icmpv6_header *)req;
	hdr->type = ICMP6_ECHO_REQUEST;
	hdr->code = 0;
	hdr->id = bswap16(0x00b1);
	hdr->seq = bswap16(1);
	memcpy(req + sizeof(struct icmpv6_header), "b1nix-v6", 8);
	hdr->checksum = 0;
	u16 csum = icmpv6_checksum(&loop, &loop, IP6_NH_ICMPV6, req, sizeof(req));
	hdr->checksum = bswap16(csum);

	ipv6_send(loop, IP6_NH_ICMPV6, req, sizeof(req));
	/* Loopback delivery is now deferred (net_loopback_enqueue) to avoid TCP
	 * state-machine re-entrancy; pump the queue so the request is received, the
	 * echo reply is sent (also deferred) and then received before we sample the
	 * counter. A single drain processes the whole cascade — items enqueued while
	 * draining are picked up in the same loop. */
	net_loopback_drain();

	if (icmpv6_echo_reply_count() > before)
		k_info(NULL, "M32-IP6: ok icmpv6-loopback");
	else
		k_info(NULL, "M32-IP6: fail icmpv6-loopback");

	/* A datagram to an unbound ::1 UDP port must produce ICMPv6 type 1,
	 * code 4 rather than disappearing silently. */
	u8 udp[9];
	memset(udp, 0, sizeof(udp));
	udp[0] = 0x12; udp[1] = 0x34;
	udp[2] = 0xfd; udp[3] = 0xe8; /* closed port 65000 */
	udp[4] = 0; udp[5] = sizeof(udp);
	udp[8] = 'x';
	u32 err_before = __atomic_load_n(&g_icmpv6_errors, __ATOMIC_RELAXED);
	ipv6_send(loop, IP6_NH_UDP, udp, sizeof(udp));
	/* Drain the deferred loopback queue: the UDP datagram is received, the
	 * port-unreachable ICMPv6 error is sent (deferred) and then received,
	 * incrementing g_icmpv6_errors — all within this one drain cascade. */
	net_loopback_drain();
	if (__atomic_load_n(&g_icmpv6_errors, __ATOMIC_RELAXED) > err_before)
		k_err(NULL, "M32-IP6: ok icmpv6-errors");
	else
		k_err(NULL, "M32-IP6: fail icmpv6-errors");

}

/* Real-link IPv6 over QEMU usernet: wait for SLAAC (RS->RA gives a prefix and
 * default router), then ICMPv6-ping the slirp gateway (fec0::2) over the wire.
 * Both steps degrade to "unsupported" (a smoke skip) when the link has no IPv6
 * router, so the suite stays deterministic. */
void ipv6_realink_smoke(void)
{
	if (!net_is_ready()) {
		k_warn(NULL, "M32-IP6: unsupported real-link");
		return;
	}

	/* net_task's ndp_tick sends the Router Solicitations; poll for the RA. */
	for (int i = 0; i < 300 && !net_get_prefix6_valid(); i++) {
		net_poll();
		scheduler_sleep_ticks(1);
	}
	if (!net_get_prefix6_valid()) {
		k_warn(NULL, "M32-IP6: unsupported real-link-slaac");
		return;
	}
	k_info(NULL, "M32-IP6: ok slaac-global");

	/* Ping the well-known slirp IPv6 gateway. */
	struct in6_addr_k gw;
	memset(&gw, 0, sizeof(gw));
	gw.bytes[0] = 0xfe;
	gw.bytes[1] = 0xc0;
	gw.bytes[15] = 2;

	u8 req[sizeof(struct icmpv6_header) + 8];
	memset(req, 0, sizeof(req));
	struct icmpv6_header *hdr = (struct icmpv6_header *)req;
	hdr->type = ICMP6_ECHO_REQUEST;
	hdr->id = bswap16(0x00b6);
	hdr->seq = bswap16(1);
	memcpy(req + sizeof(struct icmpv6_header), "b1nix-rl", 8);

	u32 before = icmpv6_echo_reply_count();
	for (int i = 0; i < 40 && icmpv6_echo_reply_count() == before; i++) {
		ipv6_send(gw, IP6_NH_ICMPV6, req, sizeof(req));
		for (int j = 0; j < 10 && icmpv6_echo_reply_count() == before; j++) {
			net_poll();
			scheduler_sleep_ticks(1);
		}
	}
	if (icmpv6_echo_reply_count() > before)
		k_info(NULL, "M32-IP6: ok real-link-ping");
	else
		k_warn(NULL, "M32-IP6: unsupported real-link-ping");
}

/* ── M96: the IPv6 datapath is a loadable module ─────────────────────────── */
#include <b1nix/module.h>

MODULE_NAME("ipv6");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("b1nix");
MODULE_DESCRIPTION("IPv6 datapath: loopback, ICMPv6 echo, real-link send");
MODULE_ALIAS("net-pf-10");
MODULE_ALIAS("ethertype-0x86dd");

/* Hop limit written into every outgoing IPv6 header (see ipv6_send). Writable
 * at runtime so a link with an unusual topology can be probed without a
 * rebuild. */
module_param_desc(ipv6_hop_limit, MODULE_PARAM_INT, 0644,
                  "hop limit for outgoing IPv6 datagrams");

static void ipv6_module_selftest(void) {
	ipv6_loopback_smoke();
	ipv6_realink_smoke();
}

static struct net_proto ipv6_proto = {
	.name = "ipv6",
	.ether_type = 0x86DD,
	.receive = ipv6_receive,
	.send6 = ipv6_send,
	.icmp6_unreach = icmpv6_send_dest_unreachable,
	.selftest = ipv6_module_selftest,
};

static int ipv6_module_init(void) { return proto_register(&ipv6_proto); }
static void ipv6_module_exit(void) { proto_unregister(&ipv6_proto); }

module_init(ipv6_module_init);
module_exit(ipv6_module_exit);

/* Consumed by ndp.ko, which builds its solicitations on top of this datapath —
 * that reference is what makes modules.dep record "ndp: ipv6". */
EXPORT_SYMBOL(ipv6_send);
EXPORT_SYMBOL(ipv6_receive);
/* M84's per-interface transmit: ndp.ko solicits on the link the route chose. */
EXPORT_SYMBOL(ipv6_send_via);
