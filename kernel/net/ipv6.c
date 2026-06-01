/* Minimal IPv6 datapath.
 *
 * This is the first slice of kernel IPv6: a loopback (::1) fast path and an
 * ICMPv6 echo (ping) responder, mirroring the IPv4 loopback path in ipv4.c.
 * Neighbor discovery, routing, real-link (ethertype 0x86DD) send, and UDP/TCP
 * over IPv6 are not implemented yet — ipv6_send() only handles ::1.
 */

#include <b1nix/net.h>
#include <b1nix/mm.h>
#include <b1nix/console.h>
#include <string.h>

#define IP6_NH_TCP    6
#define IP6_NH_UDP    17
#define IP6_NH_ICMPV6 58

#define ICMP6_ECHO_REQUEST 128
#define ICMP6_ECHO_REPLY   129

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
	}
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
		if (icmpv6_checksum(&hdr->src, &hdr->dst, IP6_NH_ICMPV6, payload,
		                    payload_len) != 0)
			return;
		icmpv6_receive(&hdr->src, &hdr->dst, payload, payload_len);
	} else if (hdr->next_header == IP6_NH_UDP) {
		udp6_receive(hdr->src, payload, payload_len);
	}
	/* IP6_NH_TCP over IPv6 is not wired yet. */
}

void ipv6_send(struct in6_addr_k dst, u8 next_header, const void *payload,
               usize size)
{
	usize total = sizeof(struct ipv6_header) + size;
	u8 *buffer = kzalloc(total);
	if (!buffer)
		return;

	struct ipv6_header *hdr = (struct ipv6_header *)buffer;
	hdr->ver_tc_hi = (u8)(6 << 4);
	hdr->payload_len = bswap16((u16)size);
	hdr->next_header = next_header;
	hdr->hop_limit = 64;
	hdr->dst = dst;
	/* No interface address is configured yet, so the only valid source is the
	 * loopback address for the ::1 path. */
	hdr->src = dst;

	memcpy(buffer + sizeof(struct ipv6_header), payload, size);

	if (in6_is_loopback(&dst)) {
		ipv6_receive(buffer, total);
		kfree(buffer);
		return;
	}

	/* Off-link IPv6 needs neighbor discovery and an ethertype-0x86DD send
	 * path, which are not implemented yet. */
	kfree(buffer);
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

	if (icmpv6_echo_reply_count() > before)
		console_write("M32-IP6: ok icmpv6-loopback\n");
	else
		console_write("M32-IP6: fail icmpv6-loopback\n");
}
