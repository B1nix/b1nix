/* IPv6 Neighbor Discovery (RFC 4861) + stateless address autoconfiguration
 * (SLAAC, RFC 4862), enough to come up on a real link (QEMU usernet):
 *
 *   - Router Solicitation (RS) -> Router Advertisement (RA) gives us the
 *     on-link prefix + default router; we form a global address from the
 *     prefix and our EUI-64 interface id.
 *   - Neighbor Solicitation / Advertisement (NS/NA) resolve an on-link IPv6
 *     address to a MAC (the IPv6 equivalent of ARP).
 *
 * ND messages are built with a zero ICMPv6 checksum; ipv6_send() fills it in
 * (it is the layer that picks the source address the checksum depends on).
 */

#include <b1nix/net.h>
#include <b1nix/console.h>
#include <b1nix/sched.h>
#include <string.h>

#define IP6_NH_ICMPV6 58
#define ICMP6_RS 133
#define ICMP6_RA 134
#define ICMP6_NS 135
#define ICMP6_NA 136
#define ND_OPT_SLLA 1 /* source link-layer address */
#define ND_OPT_TLLA 2 /* target link-layer address */
#define ND_OPT_PREFIX 3

#define NDP_CACHE_SIZE 16
struct ndp_entry {
	struct in6_addr_k ip;
	struct mac_addr mac;
	int valid;
};
static struct ndp_entry ndp_cache[NDP_CACHE_SIZE];

void ndp_init(void)
{
	for (int i = 0; i < NDP_CACHE_SIZE; i++)
		ndp_cache[i].valid = 0;
}

static int in6_eq(const struct in6_addr_k *a, const struct in6_addr_k *b)
{
	return memcmp(a->bytes, b->bytes, 16) == 0;
}

static int in6_is_zero(const struct in6_addr_k *a)
{
	for (int i = 0; i < 16; i++)
		if (a->bytes[i])
			return 0;
	return 1;
}

static void ndp_cache_put(struct in6_addr_k ip, struct mac_addr mac)
{
	for (int i = 0; i < NDP_CACHE_SIZE; i++) {
		if (ndp_cache[i].valid && in6_eq(&ndp_cache[i].ip, &ip)) {
			ndp_cache[i].mac = mac;
			return;
		}
	}
	for (int i = 0; i < NDP_CACHE_SIZE; i++) {
		if (!ndp_cache[i].valid) {
			ndp_cache[i].ip = ip;
			ndp_cache[i].mac = mac;
			ndp_cache[i].valid = 1;
			return;
		}
	}
}

/* Solicited-node multicast address ff02::1:ffXX:XXXX for a target. */
static struct in6_addr_k solicited_node(const struct in6_addr_k *t)
{
	struct in6_addr_k m;
	memset(&m, 0, sizeof(m));
	m.bytes[0] = 0xff;
	m.bytes[1] = 0x02;
	m.bytes[11] = 0x01;
	m.bytes[12] = 0xff;
	m.bytes[13] = t->bytes[13];
	m.bytes[14] = t->bytes[14];
	m.bytes[15] = t->bytes[15];
	return m;
}

static int is_one_of_ours(const struct in6_addr_k *a)
{
	struct in6_addr_k ll = net_get_ip6_ll();
	struct in6_addr_k g = net_get_ip6();
	return in6_eq(a, &ll) || (!in6_is_zero(&g) && in6_eq(a, &g));
}

/* Send a Neighbor Solicitation for `target` (to its solicited-node group),
 * carrying our source link-layer address. */
static void ndp_send_ns(struct in6_addr_k target)
{
	u8 ns[24 + 8];
	memset(ns, 0, sizeof(ns));
	ns[0] = ICMP6_NS;
	memcpy(ns + 8, target.bytes, 16);
	ns[24] = ND_OPT_SLLA;
	ns[25] = 1; /* length in units of 8 bytes */
	struct mac_addr mac = net_get_mac();
	memcpy(ns + 26, mac.bytes, 6);
	struct in6_addr_k dst = solicited_node(&target);
	ipv6_send(dst, IP6_NH_ICMPV6, ns, sizeof(ns));
}

/* Reply to a Neighbor Solicitation with a Neighbor Advertisement. */
static void ndp_send_na(struct in6_addr_k target, struct in6_addr_k to)
{
	u8 na[24 + 8];
	memset(na, 0, sizeof(na));
	na[0] = ICMP6_NA;
	na[4] = 0x60; /* Solicited (0x40) | Override (0x20) */
	memcpy(na + 8, target.bytes, 16);
	na[24] = ND_OPT_TLLA;
	na[25] = 1;
	struct mac_addr mac = net_get_mac();
	memcpy(na + 26, mac.bytes, 6);
	ipv6_send(to, IP6_NH_ICMPV6, na, sizeof(na));
}

/* Send a Router Solicitation to all-routers (ff02::2). */
static void ndp_send_rs(void)
{
	u8 rs[8 + 8];
	memset(rs, 0, sizeof(rs));
	rs[0] = ICMP6_RS;
	rs[8] = ND_OPT_SLLA;
	rs[9] = 1;
	struct mac_addr mac = net_get_mac();
	memcpy(rs + 10, mac.bytes, 6);
	struct in6_addr_k all_routers;
	memset(&all_routers, 0, sizeof(all_routers));
	all_routers.bytes[0] = 0xff;
	all_routers.bytes[1] = 0x02;
	all_routers.bytes[15] = 0x02;
	ipv6_send(all_routers, IP6_NH_ICMPV6, rs, sizeof(rs));
}

int ndp_resolve(struct in6_addr_k ip, struct mac_addr *mac)
{
	for (int i = 0; i < NDP_CACHE_SIZE; i++) {
		if (ndp_cache[i].valid && in6_eq(&ndp_cache[i].ip, &ip)) {
			*mac = ndp_cache[i].mac;
			return 1;
		}
	}
	ndp_send_ns(ip);
	return 0;
}

/* Walk ND options looking for a link-layer-address option (SLLA/TLLA); on a
 * match copy the 6-byte MAC out. Returns 1 if found. */
static int nd_find_lladdr(const u8 *opts, usize len, struct mac_addr *out)
{
	usize off = 0;
	while (off + 2 <= len) {
		u8 otype = opts[off];
		u8 olen = opts[off + 1];
		if (olen == 0)
			break;
		usize obytes = (usize)olen * 8;
		if (off + obytes > len)
			break;
		if ((otype == ND_OPT_SLLA || otype == ND_OPT_TLLA) && obytes >= 8) {
			memcpy(out->bytes, opts + off + 2, 6);
			return 1;
		}
		off += obytes;
	}
	return 0;
}

/* Walk ND options for a Prefix Information option; copy the /64 prefix out. */
static int nd_find_prefix(const u8 *opts, usize len, struct in6_addr_k *out)
{
	usize off = 0;
	while (off + 2 <= len) {
		u8 otype = opts[off];
		u8 olen = opts[off + 1];
		if (olen == 0)
			break;
		usize obytes = (usize)olen * 8;
		if (off + obytes > len)
			break;
		if (otype == ND_OPT_PREFIX && obytes >= 32) {
			memset(out, 0, sizeof(*out));
			memcpy(out->bytes, opts + off + 16, 8); /* prefix at option +16 */
			return 1;
		}
		off += obytes;
	}
	return 0;
}

static volatile int slaac_configured;

void ndp_receive(struct in6_addr_k src, struct in6_addr_k dst, u8 type,
                 const void *data, usize size)
{
	(void)dst;
	const u8 *p = data;

	if (type == ICMP6_NS) {
		if (size < 24)
			return;
		struct in6_addr_k target;
		memcpy(target.bytes, p + 8, 16);
		struct mac_addr smac;
		if (!in6_is_zero(&src) && nd_find_lladdr(p + 24, size - 24, &smac))
			ndp_cache_put(src, smac);
		if (is_one_of_ours(&target) && !in6_is_zero(&src))
			ndp_send_na(target, src);
		return;
	}

	if (type == ICMP6_NA) {
		if (size < 24)
			return;
		struct in6_addr_k target;
		memcpy(target.bytes, p + 8, 16);
		struct mac_addr tmac;
		if (nd_find_lladdr(p + 24, size - 24, &tmac))
			ndp_cache_put(target, tmac);
		return;
	}

	if (type == ICMP6_RA) {
		if (size < 16)
			return;
		/* The router's link-local source becomes the default gateway. */
		if (!in6_is_zero(&src))
			net_set_gateway6(src);
		struct mac_addr rmac;
		if (nd_find_lladdr(p + 16, size - 16, &rmac))
			ndp_cache_put(src, rmac);
		struct in6_addr_k prefix;
		if (nd_find_prefix(p + 16, size - 16, &prefix)) {
			net_set_prefix6(prefix);
			/* SLAAC: global = prefix[0:8] || EUI-64 iid (from link-local). */
			struct in6_addr_k ll = net_get_ip6_ll();
			struct in6_addr_k global;
			memcpy(global.bytes, prefix.bytes, 8);
			memcpy(global.bytes + 8, ll.bytes + 8, 8);
			net_set_ip6(global);
			if (!slaac_configured) {
				slaac_configured = 1;
				console_write("net: ipv6 SLAAC configured a global address\n");
			}
		}
		return;
	}
	/* RS (133): we are a host, not a router — ignore. */
}

void ndp_tick(u64 now_ticks)
{
	static u64 last = 0;
	static int sent = 0;
	if (!net_is_ready())
		return;
	if (!net_get_prefix6_valid() && sent < 8) {
		if (now_ticks - last >= 50) { /* ~every 500ms */
			ndp_send_rs();
			last = now_ticks;
			sent++;
		}
	}
}
