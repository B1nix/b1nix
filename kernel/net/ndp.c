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
#include <b1nix/netdev.h>
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
	/* The DHCPv6 client must exist before the first router advertisement can
	 * ask for it (the M/O flags start it). */
	dhcpv6_init();
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
static void ndp_send_ns_dev(struct in6_addr_k target, struct netdev *dev)
{
	u8 ns[24 + 8];
	memset(ns, 0, sizeof(ns));
	ns[0] = ICMP6_NS;
	memcpy(ns + 8, target.bytes, 16);
	ns[24] = ND_OPT_SLLA;
	ns[25] = 1; /* length in units of 8 bytes */
	/* M84: the solicitation must carry (and leave through) the interface the
	 * route selected, not always the active one. */
	struct mac_addr mac = dev ? dev->mac : net_get_mac();
	memcpy(ns + 26, mac.bytes, 6);
	struct in6_addr_k dst = solicited_node(&target);
	ipv6_send_via(dev, dst, IP6_NH_ICMPV6, ns, sizeof(ns));
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

int ndp_resolve_dev(struct in6_addr_k ip, struct mac_addr *mac,
                    struct netdev *dev)
{
	for (int i = 0; i < NDP_CACHE_SIZE; i++) {
		if (ndp_cache[i].valid && in6_eq(&ndp_cache[i].ip, &ip)) {
			*mac = ndp_cache[i].mac;
			return 1;
		}
	}
	ndp_send_ns_dev(ip, dev);
	return 0;
}

int ndp_resolve(struct in6_addr_k ip, struct mac_addr *mac)
{
	return ndp_resolve_dev(ip, mac, 0);
}

/* Registry hook (M96): same resolver, reached through struct net_proto so the
 * IPv6 core does not name this module. The argument order matches
 * ndp_resolve_dev; it exists only because the hook's signature is fixed. */
static int ndp_resolve_proto(struct in6_addr_k ip, struct mac_addr *mac,
                             struct netdev *dev)
{
	return ndp_resolve_dev(ip, mac, dev);
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

#define ICMP6_MLD_QUERY 130
#define ICMP6_MLD_REPORT 131
#define MLD_MAX_GROUPS 8

static struct in6_addr_k mld_groups[MLD_MAX_GROUPS];
static int mld_group_count;
static volatile u32 mld_reports;

static void mld_send_report(struct in6_addr_k group)
{
	u8 report[24];
	memset(report, 0, sizeof(report));
	report[0] = ICMP6_MLD_REPORT;
	memcpy(report + 8, group.bytes, 16);
	ipv6_send(group, IP6_NH_ICMPV6, report, sizeof(report));
	__atomic_add_fetch(&mld_reports, 1, __ATOMIC_RELAXED);
}

static void mld_join(struct in6_addr_k group)
{
	for (int i = 0; i < mld_group_count; i++)
		if (in6_eq(&mld_groups[i], &group))
			return;
	if (mld_group_count >= MLD_MAX_GROUPS)
		return;
	mld_groups[mld_group_count++] = group;
	if (net_is_ready())
		mld_send_report(group);
}

void mld_join_solicited_node(struct in6_addr_k address)
{
	mld_join(solicited_node(&address));
}

void mld_receive(struct in6_addr_k src, struct in6_addr_k dst, u8 type,
                 const void *data, usize size)
{
	(void)src;
	(void)dst;
	if (type != ICMP6_MLD_QUERY || size < 24)
		return;
	struct in6_addr_k requested;
	memcpy(requested.bytes, (const u8 *)data + 8, 16);
	for (int i = 0; i < mld_group_count; i++)
		if (in6_is_zero(&requested) || in6_eq(&requested, &mld_groups[i]))
			mld_send_report(mld_groups[i]);
}

void mld_smoke(void)
{
	struct in6_addr_k loop;
	memset(&loop, 0, sizeof(loop));
	loop.bytes[15] = 1;
	u32 before = __atomic_load_n(&mld_reports, __ATOMIC_RELAXED);
	mld_join_solicited_node(loop);
	u8 query[24];
	memset(query, 0, sizeof(query));
	query[0] = ICMP6_MLD_QUERY;
	mld_receive(loop, loop, ICMP6_MLD_QUERY, query, sizeof(query));
	if (__atomic_load_n(&mld_reports, __ATOMIC_RELAXED) > before)
		console_write("M32-IP6: ok mld\n");
	else
		console_write("M32-IP6: fail mld\n");
}

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
		/* RFC 4861 flags byte: M (0x80) means addresses come from DHCPv6, O
		 * (0x40) means other configuration (resolvers) does. Either one starts
		 * the stateful client. */
		u8 ra_flags = p[5];
		if (ra_flags & 0xC0)
			dhcpv6_start();
		/* The router's link-local source becomes the default gateway. */
		if (!in6_is_zero(&src))
			net_set_gateway6(src);
		int ra_prefix_valid = 0;
		struct in6_addr_k ra_prefix;
		memset(&ra_prefix, 0, sizeof(ra_prefix));
		struct mac_addr rmac;
		if (nd_find_lladdr(p + 16, size - 16, &rmac))
			ndp_cache_put(src, rmac);
		struct in6_addr_k prefix;
		if (nd_find_prefix(p + 16, size - 16, &prefix)) {
			net_set_prefix6(prefix);
			ra_prefix_valid = 1;
			ra_prefix = prefix;
			/* SLAAC: global = prefix[0:8] || EUI-64 iid (from link-local). */
			struct in6_addr_k ll = net_get_ip6_ll();
			struct in6_addr_k global;
			memcpy(global.bytes, prefix.bytes, 8);
			memcpy(global.bytes + 8, ll.bytes + 8, 8);
			net_set_ip6(global);
			mld_join_solicited_node(global);
			if (!slaac_configured) {
				slaac_configured = 1;
				console_write("net: ipv6 SLAAC configured a global address\n");
			}
		}
		/* M84: feed the advertisement into the IPv6 FIB — the on-link /64
		 * prefix plus a default route via the advertising router. This is what
		 * ipv6_link_output() now consults for every off-link destination. */
		{
			struct in6_addr_k router = in6_is_zero(&src) ? ra_prefix : src;
			struct in6_addr_k zero;
			memset(&zero, 0, sizeof(zero));
			if (ra_prefix_valid)
				route6_configure_interface(ra_prefix, 64,
				                           in6_is_zero(&src) ? zero : router);
			else if (!in6_is_zero(&src))
				route6_configure_interface(zero, 0, router);
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

/* ── M96: Neighbour Discovery is a loadable module ───────────────────────── */
#include <b1nix/module.h>
#include <b1nix/netproto.h>

MODULE_NAME("ndp");
MODULE_LICENSE("MIT");
MODULE_AUTHOR("b1nix");
MODULE_DESCRIPTION("IPv6 Neighbour Discovery and SLAAC");
MODULE_ALIAS("net-ipv6-nd");

static void ndp_module_reset(void) { ndp_init(); }

/* ipv6.ko hands every ICMPv6 control message here; sort it by type. */
static void ndp_icmp6_receive(struct in6_addr_k src, struct in6_addr_k dst,
                              u8 type, const void *data, usize size)
{
	if (type >= ICMP6_RS && type <= ICMP6_NA)
		ndp_receive(src, dst, type, data, size);
	else
		mld_receive(src, dst, type, data, size);
}

static struct net_proto ndp_proto = {
	.name = "ndp",
	.resolve6 = ndp_resolve_proto,
	.icmp6 = ndp_icmp6_receive,
	.tick = ndp_tick,
	.reset = ndp_module_reset,
	.selftest = mld_smoke,
};

static int ndp_module_init(void) {
	ndp_init();
	return proto_register(&ndp_proto);
}

static void ndp_module_exit(void) { proto_unregister(&ndp_proto); }

module_init(ndp_module_init);
module_exit(ndp_module_exit);
