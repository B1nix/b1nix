/* M84: forwarding information base (FIB), IPv4 and IPv6.
 *
 * Before this the IP layer had no routing table at all: ipv4_send() assumed a
 * /24 on-link prefix and sent everything else to the single DHCP-learnt
 * gateway, ipv6_link_output() used the one SLAAC-learnt router, /proc/net/route
 * synthesised two lines, and SIOCADDRT/SIOCDELRT were accepted as no-ops. That
 * breaks any topology that is not a flat /24 with one router — a /16 usernet, a
 * second on-link prefix, a host route, a per-destination gateway installed by
 * `route add`, or two NICs with different upstreams.
 *
 * Both tables are small flat arrays walked with longest-prefix-match: longest
 * mask wins, lowest metric breaks ties, and remaining equal-cost routes are
 * load-shared (ECMP) by hashing the destination — per-destination hashing, so
 * a flow always leaves through the same next hop and never reorders. Every
 * entry carries an output interface index (0 = "whatever is active"), which is
 * what makes the table per-interface rather than global.
 *
 * IPv4 addresses are kept in host byte order (a.b.c.d -> 0xAABBCCDD) so prefix
 * arithmetic is plain integer masking; IPv6 keeps the 16 network-order bytes
 * and compares bitwise. Conversion happens at the edges.
 */

#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/spinlock.h>
#include <b1nix/console.h>
#include <b1nix/vfs.h>
#include <string.h>

#define ROUTE_MAX_ENTRIES 64
#define ROUTE6_MAX_ENTRIES 64
#define ROUTE_MAX_RULES 16

/* Policy routing: several tables, selected by rules. The table ids match
 * Linux's well-known ones so `table main` means the same thing here. */
#define RT_TABLE_MAIN 254
#define RT_RULE_PRIO_MAIN 32766

struct route_entry {
	int used;
	u32 dst;      /* network address, host order */
	u32 mask;     /* netmask, host order */
	u32 gateway;  /* 0 == on-link (destination is directly reachable) */
	u16 flags;    /* RTF_* */
	u16 metric;
	int oif;      /* output interface index, 0 = unspecified */
	u32 table;    /* routing table id (RT_TABLE_MAIN unless policy says else) */
	u8 dynamic;   /* installed by DHCP/autoconf — dropped on lease loss */
};

struct route6_entry {
	int used;
	u8 dst[16];
	u8 gateway[16]; /* all-zero == on-link */
	u8 plen;        /* prefix length in bits */
	u16 flags;
	u16 metric;
	int oif;
	u32 table;
	u8 dynamic;
};

/* One policy rule. Rules are consulted in ascending priority; the first whose
 * selectors match and whose table produces a route wins. A NULL selector (mask
 * 0 / iif 0) matches everything, which is how the default "everyone uses the
 * main table" rule is expressed. */
struct route_rule {
	int used;
	u8 family;      /* 4 or 6 */
	u32 prio;
	u32 src;        /* IPv4 source prefix, host order */
	u32 src_mask;
	u8 src6[16];
	u8 src6_plen;
	int iif;        /* input/originating interface index, 0 = any */
	u32 table;
};

static struct route_rule rules[ROUTE_MAX_RULES];

static struct route_entry routes[ROUTE_MAX_ENTRIES];
static struct route6_entry routes6[ROUTE6_MAX_ENTRIES];
static spinlock_t route_lock = SPINLOCK_INIT;

u32 route_ipv4_to_host(struct ipv4_addr a)
{
	return ((u32)a.bytes[0] << 24) | ((u32)a.bytes[1] << 16) |
	       ((u32)a.bytes[2] << 8) | (u32)a.bytes[3];
}

struct ipv4_addr route_host_to_ipv4(u32 v)
{
	struct ipv4_addr a;
	a.bytes[0] = (u8)(v >> 24);
	a.bytes[1] = (u8)(v >> 16);
	a.bytes[2] = (u8)(v >> 8);
	a.bytes[3] = (u8)v;
	return a;
}

/* Number of set bits in a mask — the prefix length used to rank matches. A
 * non-contiguous mask still ranks sanely because only its width matters. */
static int mask_bits(u32 mask)
{
	int n = 0;
	for (int i = 0; i < 32; i++) {
		if (mask & (1u << i))
			n++;
	}
	return n;
}

/* Per-destination ECMP selector. Any cheap avalanche will do; what matters is
 * that the same destination always lands on the same next hop. */
static u32 route_hash32(u32 v)
{
	v ^= v >> 16;
	v *= 0x7FEB352Du;
	v ^= v >> 15;
	v *= 0x846CA68Bu;
	v ^= v >> 16;
	return v;
}

/* Flow hash over the 5-tuple. Source and destination are mixed symmetrically
 * enough for ECMP purposes: what matters is that every packet of one flow
 * hashes identically, so a flow never straddles two next hops and never
 * reorders. */
u32 route_flow_hash(const void *src_addr, const void *dst_addr, usize addr_len,
                    u8 proto, u16 sport, u16 dport)
{
	const u8 *s = src_addr;
	const u8 *d = dst_addr;
	u32 h = 0x9E3779B9u;

	for (usize i = 0; i < addr_len; i++)
		h = route_hash32(h ^ ((u32)s[i] << 8) ^ d[i]);
	h = route_hash32(h ^ ((u32)proto << 24) ^ ((u32)sport << 8) ^ dport);
	return h ? h : 1; /* 0 means "no flow information" to the lookup */
}

static int route_iface_index(const char *iface)
{
	if (!iface || !iface[0])
		return 0;
	return netdev_index_by_name(iface);
}

/* ── IPv4 table ──────────────────────────────────────────────────────────── */

static struct route_entry *route_find_locked(u32 dst, u32 mask, u32 gw, int oif,
                                             u32 table, int match_gw)
{
	for (int i = 0; i < ROUTE_MAX_ENTRIES; i++) {
		if (!routes[i].used)
			continue;
		if (routes[i].table != table)
			continue;
		if (routes[i].dst != (dst & mask) || routes[i].mask != mask)
			continue;
		if (match_gw && (routes[i].gateway != gw || routes[i].oif != oif))
			continue;
		return &routes[i];
	}
	return 0;
}

static int route_add_locked(u32 dst, u32 mask, u32 gw, u16 flags, u16 metric,
                            int oif, u32 table, int dynamic)
{
	dst &= mask;

	/* Re-adding the exact same route (prefix, next hop and interface) updates
	 * it in place; a different next hop for the same prefix is a second,
	 * coexisting route ranked by metric — that is how a host holds two default
	 * gateways, and what makes ECMP possible. A DHCP renew that changes the
	 * gateway does not leave the old one behind because
	 * route_configure_interface() drops the dynamic entries first. */
	struct route_entry *e = route_find_locked(dst, mask, gw, oif, table, 1);
	if (!e) {
		for (int i = 0; i < ROUTE_MAX_ENTRIES; i++) {
			if (!routes[i].used) {
				e = &routes[i];
				break;
			}
		}
	}
	if (!e)
		return -1;

	memset(e, 0, sizeof(*e));
	e->used = 1;
	e->dst = dst;
	e->mask = mask;
	e->gateway = gw;
	e->flags = (u16)(flags | RTF_UP);
	if (gw != 0)
		e->flags |= RTF_GATEWAY;
	if (mask == 0xFFFFFFFFu)
		e->flags |= RTF_HOST;
	e->metric = metric;
	e->oif = oif;
	e->table = table;
	e->dynamic = (u8)(dynamic ? 1 : 0);
	return 0;
}

int route_add_table(u32 dst, u32 mask, u32 gw, u16 flags, u16 metric, int oif,
                    u32 table)
{
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	int rc = route_add_locked(dst, mask, gw, flags, metric, oif, table, 0);
	spin_unlock_irqrestore(&route_lock, f);
	return rc;
}

int route_add_oif(u32 dst, u32 mask, u32 gw, u16 flags, u16 metric, int oif)
{
	return route_add_table(dst, mask, gw, flags, metric, oif, RT_TABLE_MAIN);
}

int route_add(u32 dst, u32 mask, u32 gw, u16 flags, u16 metric,
              const char *iface)
{
	return route_add_oif(dst, mask, gw, flags, metric, route_iface_index(iface));
}

int route_del_table(u32 dst, u32 mask, u32 gw, u32 table)
{
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	/* Match on gateway first (delete the exact route the caller named); fall
	 * back to prefix-only so `route del -net X` works without repeating the
	 * next hop. */
	struct route_entry *e = 0;
	if (gw != 0) {
		for (int i = 0; i < ROUTE_MAX_ENTRIES && !e; i++) {
			if (routes[i].used && routes[i].table == table &&
			    routes[i].mask == mask &&
			    routes[i].dst == (dst & mask) && routes[i].gateway == gw)
				e = &routes[i];
		}
	}
	if (!e)
		e = route_find_locked(dst, mask, 0, 0, table, 0);
	if (!e) {
		spin_unlock_irqrestore(&route_lock, f);
		return -1;
	}
	memset(e, 0, sizeof(*e));
	spin_unlock_irqrestore(&route_lock, f);
	return 0;
}

int route_del(u32 dst, u32 mask, u32 gw)
{
	return route_del_table(dst, mask, gw, RT_TABLE_MAIN);
}

void route_flush_dynamic(void)
{
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	for (int i = 0; i < ROUTE_MAX_ENTRIES; i++) {
		if (routes[i].used && routes[i].dynamic)
			memset(&routes[i], 0, sizeof(routes[i]));
	}
	spin_unlock_irqrestore(&route_lock, f);
}

void route_flush_all(void)
{
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	memset(routes, 0, sizeof(routes));
	spin_unlock_irqrestore(&route_lock, f);
}

/* Install the interface's own prefix (on-link) plus the default route. Called
 * by DHCP on every bind so the FIB tracks the lease. A zero mask falls back to
 * the class-based guess only as a last resort — a server that omits option 1
 * is broken, but we still have to route. */
void route_configure_interface(struct ipv4_addr ip, struct ipv4_addr mask,
                               struct ipv4_addr gw)
{
	u32 h_ip = route_ipv4_to_host(ip);
	u32 h_mask = route_ipv4_to_host(mask);
	u32 h_gw = route_ipv4_to_host(gw);
	int oif = netdev_index_of(netdev_active());

	if (h_mask == 0 && h_ip != 0) {
		u8 first = ip.bytes[0];
		if (first < 128)
			h_mask = 0xFF000000u;
		else if (first < 192)
			h_mask = 0xFFFF0000u;
		else
			h_mask = 0xFFFFFF00u;
	}

	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	for (int i = 0; i < ROUTE_MAX_ENTRIES; i++) {
		if (routes[i].used && routes[i].dynamic)
			memset(&routes[i], 0, sizeof(routes[i]));
	}
	if (h_ip != 0)
		route_add_locked(h_ip & h_mask, h_mask, 0, RTF_UP, 0, oif,
		                 RT_TABLE_MAIN, 1);
	if (h_gw != 0)
		route_add_locked(0, 0, h_gw, RTF_UP | RTF_GATEWAY, 0, oif,
		                 RT_TABLE_MAIN, 1);
	spin_unlock_irqrestore(&route_lock, f);
}

/* Longest-prefix-match inside one table. Two passes under the same lock: the
 * first finds the best (prefix length, metric) and counts how many equal-cost
 * next hops share it, the second picks the flow's hop out of that set. Two
 * passes instead of a candidate array means there is no cap on the width of an
 * ECMP group — it is bounded only by the table itself. */
static int route_lookup_table_locked(u32 table, u32 d, u32 flow,
                                     struct ipv4_addr dst,
                                     struct ipv4_addr *nexthop, u16 *flags,
                                     int *oif)
{
	int best_bits = -1;
	u16 best_metric = 0;
	u32 count = 0;

	for (int i = 0; i < ROUTE_MAX_ENTRIES; i++) {
		if (!routes[i].used || routes[i].table != table)
			continue;
		if (!(routes[i].flags & RTF_UP))
			continue;
		if ((d & routes[i].mask) != routes[i].dst)
			continue;
		int bits = mask_bits(routes[i].mask);
		if (bits > best_bits ||
		    (bits == best_bits && routes[i].metric < best_metric)) {
			best_bits = bits;
			best_metric = routes[i].metric;
			count = 1;
		} else if (bits == best_bits && routes[i].metric == best_metric) {
			count++;
		}
	}
	if (count == 0)
		return 0;

	u32 pick = count == 1 ? 0 : route_hash32(flow ? flow : d) % count;
	u32 seen = 0;
	for (int i = 0; i < ROUTE_MAX_ENTRIES; i++) {
		if (!routes[i].used || routes[i].table != table)
			continue;
		if (!(routes[i].flags & RTF_UP))
			continue;
		if ((d & routes[i].mask) != routes[i].dst)
			continue;
		if (mask_bits(routes[i].mask) != best_bits ||
		    routes[i].metric != best_metric)
			continue;
		if (seen++ != pick)
			continue;
		if (nexthop)
			*nexthop = routes[i].gateway ? route_host_to_ipv4(routes[i].gateway)
			                             : dst;
		if (flags)
			*flags = routes[i].flags;
		if (oif)
			*oif = routes[i].oif;
		return 1;
	}
	return 0;
}

static int rule_matches_v4(const struct route_rule *r, u32 src, int iif)
{
	if (r->iif && iif && r->iif != iif)
		return 0;
	if (r->src_mask && (src & r->src_mask) != r->src)
		return 0;
	return 1;
}

/* Walk the rules in ascending priority and return the first route any matching
 * rule's table produces — the policy-routing lookup proper. */
int route_lookup_ex(struct ipv4_addr src, struct ipv4_addr dst, u32 flow,
                    int iif, struct ipv4_addr *nexthop, u16 *flags, int *oif)
{
	u32 d = route_ipv4_to_host(dst);
	u32 s4 = route_ipv4_to_host(src);
	int found = 0;

	u64 f;
	spin_lock_irqsave(&route_lock, &f);

	u32 last_prio = 0;
	int first = 1;
	for (;;) {
		/* Select the next rule by priority without sorting the array. */
		int idx = -1;
		u32 best_prio = 0;
		for (int i = 0; i < ROUTE_MAX_RULES; i++) {
			if (!rules[i].used || rules[i].family != 4)
				continue;
			if (!first && rules[i].prio <= last_prio)
				continue;
			if (idx < 0 || rules[i].prio < best_prio) {
				idx = i;
				best_prio = rules[i].prio;
			}
		}
		if (idx < 0)
			break;
		last_prio = rules[idx].prio;
		first = 0;

		if (!rule_matches_v4(&rules[idx], s4, iif))
			continue;
		if (route_lookup_table_locked(rules[idx].table, d, flow, dst, nexthop,
		                              flags, oif)) {
			found = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&route_lock, f);
	return found;
}

int route_lookup_flow(struct ipv4_addr dst, u32 flow, struct ipv4_addr *nexthop,
                      u16 *flags, int *oif)
{
	return route_lookup_ex(net_get_ip(), dst, flow, 0, nexthop, flags, oif);
}

int route_lookup(struct ipv4_addr dst, struct ipv4_addr *nexthop, u16 *flags,
                 int *oif)
{
	return route_lookup_flow(dst, 0, nexthop, flags, oif);
}

usize route_snapshot(struct route_info *out, usize max)
{
	usize n = 0;
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	for (int i = 0; i < ROUTE_MAX_ENTRIES && n < max; i++) {
		if (!routes[i].used)
			continue;
		out[n].dst = routes[i].dst;
		out[n].mask = routes[i].mask;
		out[n].gateway = routes[i].gateway;
		out[n].flags = routes[i].flags;
		out[n].metric = routes[i].metric;
		out[n].oif = routes[i].oif;
		out[n].table = routes[i].table;
		netdev_ifname(routes[i].oif ? routes[i].oif : 1, out[n].iface,
		              sizeof(out[n].iface));
		n++;
	}
	spin_unlock_irqrestore(&route_lock, f);
	return n;
}

/* ── IPv6 table ──────────────────────────────────────────────────────────── */

static int in6_prefix_eq(const u8 *a, const u8 *b, u8 plen)
{
	if (plen > 128)
		plen = 128;
	u8 whole = (u8)(plen / 8);
	u8 rest = (u8)(plen % 8);
	if (whole && memcmp(a, b, whole) != 0)
		return 0;
	if (rest) {
		u8 m = (u8)(0xFF << (8 - rest));
		if ((a[whole] & m) != (b[whole] & m))
			return 0;
	}
	return 1;
}

static void in6_apply_prefix(u8 *addr, u8 plen)
{
	if (plen > 128)
		plen = 128;
	u8 whole = (u8)(plen / 8);
	u8 rest = (u8)(plen % 8);
	if (rest) {
		addr[whole] &= (u8)(0xFF << (8 - rest));
		whole++;
	}
	for (int i = whole; i < 16; i++)
		addr[i] = 0;
}

static int in6_is_zero_bytes(const u8 *a)
{
	for (int i = 0; i < 16; i++) {
		if (a[i])
			return 0;
	}
	return 1;
}

static struct route6_entry *route6_find_locked(const u8 *dst, u8 plen,
                                               const u8 *gw, int oif,
                                               u32 table, int match_gw)
{
	u8 net[16];
	memcpy(net, dst, 16);
	in6_apply_prefix(net, plen);
	for (int i = 0; i < ROUTE6_MAX_ENTRIES; i++) {
		if (!routes6[i].used || routes6[i].table != table)
			continue;
		if (routes6[i].plen != plen || memcmp(routes6[i].dst, net, 16) != 0)
			continue;
		if (match_gw &&
		    (memcmp(routes6[i].gateway, gw, 16) != 0 || routes6[i].oif != oif))
			continue;
		return &routes6[i];
	}
	return 0;
}

static int route6_add_locked(const u8 *dst, u8 plen, const u8 *gw, u16 flags,
                             u16 metric, int oif, u32 table, int dynamic)
{
	struct route6_entry *e = route6_find_locked(dst, plen, gw, oif, table, 1);
	if (!e) {
		for (int i = 0; i < ROUTE6_MAX_ENTRIES; i++) {
			if (!routes6[i].used) {
				e = &routes6[i];
				break;
			}
		}
	}
	if (!e)
		return -1;

	memset(e, 0, sizeof(*e));
	e->used = 1;
	memcpy(e->dst, dst, 16);
	in6_apply_prefix(e->dst, plen);
	memcpy(e->gateway, gw, 16);
	e->plen = plen > 128 ? 128 : plen;
	e->flags = (u16)(flags | RTF_UP);
	if (!in6_is_zero_bytes(gw))
		e->flags |= RTF_GATEWAY;
	if (e->plen == 128)
		e->flags |= RTF_HOST;
	e->metric = metric;
	e->oif = oif;
	e->table = table;
	e->dynamic = (u8)(dynamic ? 1 : 0);
	return 0;
}

int route6_add_table(struct in6_addr_k dst, u8 plen, struct in6_addr_k gw,
                     u16 flags, u16 metric, int oif, u32 table)
{
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	int rc = route6_add_locked(dst.bytes, plen, gw.bytes, flags, metric, oif,
	                           table, 0);
	spin_unlock_irqrestore(&route_lock, f);
	return rc;
}

int route6_add(struct in6_addr_k dst, u8 plen, struct in6_addr_k gw, u16 flags,
               u16 metric, int oif)
{
	return route6_add_table(dst, plen, gw, flags, metric, oif, RT_TABLE_MAIN);
}

int route6_del_table(struct in6_addr_k dst, u8 plen, struct in6_addr_k gw,
                     u32 table)
{
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	struct route6_entry *e = 0;
	if (!in6_is_zero_bytes(gw.bytes)) {
		u8 net[16];
		memcpy(net, dst.bytes, 16);
		in6_apply_prefix(net, plen);
		for (int i = 0; i < ROUTE6_MAX_ENTRIES && !e; i++) {
			if (routes6[i].used && routes6[i].table == table &&
			    routes6[i].plen == plen &&
			    memcmp(routes6[i].dst, net, 16) == 0 &&
			    memcmp(routes6[i].gateway, gw.bytes, 16) == 0)
				e = &routes6[i];
		}
	}
	if (!e) {
		struct in6_addr_k zero;
		memset(&zero, 0, sizeof(zero));
		e = route6_find_locked(dst.bytes, plen, zero.bytes, 0, table, 0);
	}
	if (!e) {
		spin_unlock_irqrestore(&route_lock, f);
		return -1;
	}
	memset(e, 0, sizeof(*e));
	spin_unlock_irqrestore(&route_lock, f);
	return 0;
}

int route6_del(struct in6_addr_k dst, u8 plen, struct in6_addr_k gw)
{
	return route6_del_table(dst, plen, gw, RT_TABLE_MAIN);
}

void route6_flush_dynamic(void)
{
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	for (int i = 0; i < ROUTE6_MAX_ENTRIES; i++) {
		if (routes6[i].used && routes6[i].dynamic)
			memset(&routes6[i], 0, sizeof(routes6[i]));
	}
	spin_unlock_irqrestore(&route_lock, f);
}

void route6_flush_all(void)
{
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	memset(routes6, 0, sizeof(routes6));
	spin_unlock_irqrestore(&route_lock, f);
}

/* ── Policy rules ────────────────────────────────────────────────────────── */

int route_rule_add(u8 family, u32 prio, struct ipv4_addr src4,
                   struct ipv4_addr mask4, struct in6_addr_k src6, u8 plen6,
                   int iif, u32 table)
{
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	struct route_rule *r = 0;
	for (int i = 0; i < ROUTE_MAX_RULES; i++) {
		if (rules[i].used && rules[i].family == family &&
		    rules[i].prio == prio) {
			r = &rules[i]; /* same priority replaces */
			break;
		}
	}
	if (!r) {
		for (int i = 0; i < ROUTE_MAX_RULES; i++) {
			if (!rules[i].used) {
				r = &rules[i];
				break;
			}
		}
	}
	if (!r) {
		spin_unlock_irqrestore(&route_lock, f);
		return -1;
	}
	memset(r, 0, sizeof(*r));
	r->used = 1;
	r->family = family;
	r->prio = prio;
	r->src = route_ipv4_to_host(src4) & route_ipv4_to_host(mask4);
	r->src_mask = route_ipv4_to_host(mask4);
	memcpy(r->src6, src6.bytes, 16);
	in6_apply_prefix(r->src6, plen6);
	r->src6_plen = plen6;
	r->iif = iif;
	r->table = table;
	spin_unlock_irqrestore(&route_lock, f);
	return 0;
}

int route_rule_del(u8 family, u32 prio)
{
	u64 f;
	int rc = -1;
	spin_lock_irqsave(&route_lock, &f);
	for (int i = 0; i < ROUTE_MAX_RULES; i++) {
		if (rules[i].used && rules[i].family == family &&
		    rules[i].prio == prio) {
			memset(&rules[i], 0, sizeof(rules[i]));
			rc = 0;
		}
	}
	spin_unlock_irqrestore(&route_lock, f);
	return rc;
}

usize route_rule_snapshot(struct route_rule_info *out, usize max)
{
	usize n = 0;
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	for (int i = 0; i < ROUTE_MAX_RULES && n < max; i++) {
		if (!rules[i].used)
			continue;
		out[n].family = rules[i].family;
		out[n].prio = rules[i].prio;
		out[n].src = rules[i].src;
		out[n].src_mask = rules[i].src_mask;
		memcpy(out[n].src6, rules[i].src6, 16);
		out[n].src6_plen = rules[i].src6_plen;
		out[n].iif = rules[i].iif;
		out[n].table = rules[i].table;
		n++;
	}
	spin_unlock_irqrestore(&route_lock, f);
	return n;
}

/* The default policy: everything looks in the main table. Reinstalled by
 * route_init() so a flush can never leave the host unable to route. */
void route_rules_reset(void)
{
	struct ipv4_addr z4 = {{0, 0, 0, 0}};
	struct in6_addr_k z6;
	memset(&z6, 0, sizeof(z6));

	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	memset(rules, 0, sizeof(rules));
	spin_unlock_irqrestore(&route_lock, f);

	route_rule_add(4, RT_RULE_PRIO_MAIN, z4, z4, z6, 0, 0, RT_TABLE_MAIN);
	route_rule_add(6, RT_RULE_PRIO_MAIN, z4, z4, z6, 0, 0, RT_TABLE_MAIN);
}

void route_init(void)
{
	route_flush_all();
	route_rules_reset();
}

/* fe80::/10 is on-link on every interface by definition, and ::1/128 is the
 * loopback host route. Installed once at init so link-local traffic and NDP
 * work before (and without) any router advertisement. */
void route6_init(void)
{
	struct in6_addr_k p, zero;
	memset(&zero, 0, sizeof(zero));

	memset(&p, 0, sizeof(p));
	p.bytes[0] = 0xfe;
	p.bytes[1] = 0x80;
	route6_add(p, 10, zero, RTF_UP, 0, 0);

	memset(&p, 0, sizeof(p));
	p.bytes[15] = 1;
	route6_add(p, 128, zero, RTF_UP, 0, 0);

	/* Link-local multicast (ff02::/16) is delivered by mapping the address to
	 * a 33:33 MAC, but it still needs a route to be reachable at all. */
	memset(&p, 0, sizeof(p));
	p.bytes[0] = 0xff;
	p.bytes[1] = 0x02;
	route6_add(p, 16, zero, RTF_UP, 0, 0);
}

/* SLAAC result: the advertised on-link prefix plus the advertising router as
 * the default route. Replaces the previously autoconfigured pair. */
void route6_configure_interface(struct in6_addr_k prefix, u8 plen,
                                struct in6_addr_k router)
{
	int oif = netdev_index_of(netdev_active());
	struct in6_addr_k zero, any;
	memset(&zero, 0, sizeof(zero));
	memset(&any, 0, sizeof(any));

	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	for (int i = 0; i < ROUTE6_MAX_ENTRIES; i++) {
		if (routes6[i].used && routes6[i].dynamic)
			memset(&routes6[i], 0, sizeof(routes6[i]));
	}
	if (!in6_is_zero_bytes(prefix.bytes))
		route6_add_locked(prefix.bytes, plen, zero.bytes, RTF_UP, 0, oif,
		                  RT_TABLE_MAIN, 1);
	if (!in6_is_zero_bytes(router.bytes))
		route6_add_locked(any.bytes, 0, router.bytes, RTF_UP | RTF_GATEWAY, 0,
		                  oif, RT_TABLE_MAIN, 1);
	spin_unlock_irqrestore(&route_lock, f);
}

static int route6_lookup_table_locked(u32 table, struct in6_addr_k dst,
                                      u32 flow, struct in6_addr_k *nexthop,
                                      u16 *flags, int *oif)
{
	int best_plen = -1;
	u16 best_metric = 0;
	u32 count = 0;

	for (int i = 0; i < ROUTE6_MAX_ENTRIES; i++) {
		if (!routes6[i].used || routes6[i].table != table)
			continue;
		if (!(routes6[i].flags & RTF_UP))
			continue;
		if (!in6_prefix_eq(routes6[i].dst, dst.bytes, routes6[i].plen))
			continue;
		if ((int)routes6[i].plen > best_plen ||
		    ((int)routes6[i].plen == best_plen &&
		     routes6[i].metric < best_metric)) {
			best_plen = routes6[i].plen;
			best_metric = routes6[i].metric;
			count = 1;
		} else if ((int)routes6[i].plen == best_plen &&
		           routes6[i].metric == best_metric) {
			count++;
		}
	}
	if (count == 0)
		return 0;

	u32 h = flow ? flow
	             : (((u32)dst.bytes[12] << 24) | ((u32)dst.bytes[13] << 16) |
	                ((u32)dst.bytes[14] << 8) | dst.bytes[15]);
	u32 pick = count == 1 ? 0 : route_hash32(h) % count;
	u32 seen = 0;
	for (int i = 0; i < ROUTE6_MAX_ENTRIES; i++) {
		if (!routes6[i].used || routes6[i].table != table)
			continue;
		if (!(routes6[i].flags & RTF_UP))
			continue;
		if (!in6_prefix_eq(routes6[i].dst, dst.bytes, routes6[i].plen))
			continue;
		if ((int)routes6[i].plen != best_plen ||
		    routes6[i].metric != best_metric)
			continue;
		if (seen++ != pick)
			continue;
		if (nexthop) {
			if (in6_is_zero_bytes(routes6[i].gateway))
				*nexthop = dst;
			else
				memcpy(nexthop->bytes, routes6[i].gateway, 16);
		}
		if (flags)
			*flags = routes6[i].flags;
		if (oif)
			*oif = routes6[i].oif;
		return 1;
	}
	return 0;
}

int route6_lookup_ex(struct in6_addr_k src, struct in6_addr_k dst, u32 flow,
                     int iif, struct in6_addr_k *nexthop, u16 *flags, int *oif)
{
	int found = 0;
	u64 f;
	spin_lock_irqsave(&route_lock, &f);

	u32 last_prio = 0;
	int first = 1;
	for (;;) {
		int idx = -1;
		u32 best_prio = 0;
		for (int i = 0; i < ROUTE_MAX_RULES; i++) {
			if (!rules[i].used || rules[i].family != 6)
				continue;
			if (!first && rules[i].prio <= last_prio)
				continue;
			if (idx < 0 || rules[i].prio < best_prio) {
				idx = i;
				best_prio = rules[i].prio;
			}
		}
		if (idx < 0)
			break;
		last_prio = rules[idx].prio;
		first = 0;

		if (rules[idx].iif && iif && rules[idx].iif != iif)
			continue;
		if (rules[idx].src6_plen &&
		    !in6_prefix_eq(rules[idx].src6, src.bytes, rules[idx].src6_plen))
			continue;
		if (route6_lookup_table_locked(rules[idx].table, dst, flow, nexthop,
		                               flags, oif)) {
			found = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&route_lock, f);
	return found;
}

int route6_lookup_flow(struct in6_addr_k dst, u32 flow,
                       struct in6_addr_k *nexthop, u16 *flags, int *oif)
{
	return route6_lookup_ex(net_get_ip6(), dst, flow, 0, nexthop, flags, oif);
}

int route6_lookup(struct in6_addr_k dst, struct in6_addr_k *nexthop, u16 *flags,
                  int *oif)
{
	return route6_lookup_flow(dst, 0, nexthop, flags, oif);
}

usize route6_snapshot(struct route6_info *out, usize max)
{
	usize n = 0;
	u64 f;
	spin_lock_irqsave(&route_lock, &f);
	for (int i = 0; i < ROUTE6_MAX_ENTRIES && n < max; i++) {
		if (!routes6[i].used)
			continue;
		memcpy(out[n].dst, routes6[i].dst, 16);
		memcpy(out[n].gateway, routes6[i].gateway, 16);
		out[n].plen = routes6[i].plen;
		out[n].flags = routes6[i].flags;
		out[n].metric = routes6[i].metric;
		out[n].oif = routes6[i].oif;
		out[n].table = routes6[i].table;
		netdev_ifname(routes6[i].oif ? routes6[i].oif : 1, out[n].iface,
		              sizeof(out[n].iface));
		n++;
	}
	spin_unlock_irqrestore(&route_lock, f);
	return n;
}

/* ── M84 self-test ─────────────────────────────────────────────────────────
 * Exercises both tables directly: longest-prefix-match ordering, host routes,
 * gateway vs on-link next hop, metric tie-break, ECMP stability, per-interface
 * routes and deletion. Runs on the live tables, so it saves and restores
 * whatever DHCP/SLAAC installed. */
static void route_smoke_v4(void)
{
	int ok = 1;
	struct ipv4_addr nh;
	u16 fl;
	int oif;

	/* 10.0.0.0/8 via 192.168.1.1, plus a more specific 10.1.0.0/16 on-link
	 * and a host route 10.1.2.3/32 via 192.168.1.9. */
	route_add_oif(0x0A000000u, 0xFF000000u, 0xC0A80101u, RTF_UP, 0, 0);
	route_add_oif(0x0A010000u, 0xFFFF0000u, 0, RTF_UP, 0, 0);
	route_add_oif(0x0A010203u, 0xFFFFFFFFu, 0xC0A80109u, RTF_UP, 0, 0);
	route_add_oif(0, 0, 0xC0A801FEu, RTF_UP, 0, 0);

	/* 10.9.9.9 matches only the /8 -> gateway 192.168.1.1. */
	struct ipv4_addr a = {{10, 9, 9, 9}};
	if (!route_lookup(a, &nh, &fl, &oif) || nh.bytes[0] != 192 ||
	    nh.bytes[3] != 1 || !(fl & RTF_GATEWAY))
		ok = 0;

	/* 10.1.5.5 matches the /16 -> on-link, next hop is the destination. */
	struct ipv4_addr b = {{10, 1, 5, 5}};
	if (!route_lookup(b, &nh, &fl, &oif) || memcmp(nh.bytes, b.bytes, 4) != 0 ||
	    (fl & RTF_GATEWAY))
		ok = 0;

	/* 10.1.2.3 matches the /32 host route -> gateway 192.168.1.9. */
	struct ipv4_addr c = {{10, 1, 2, 3}};
	if (!route_lookup(c, &nh, &fl, &oif) || nh.bytes[3] != 9 ||
	    !(fl & RTF_HOST))
		ok = 0;

	/* Anything else falls to the default route. */
	struct ipv4_addr d = {{8, 8, 8, 8}};
	if (!route_lookup(d, &nh, &fl, &oif) || nh.bytes[3] != 254)
		ok = 0;

	console_write(ok ? "M84-ROUTE: ok lpm\n" : "M84-ROUTE: FAIL lpm\n");

	/* Deleting the host route must fall back to the /16. */
	route_del(0x0A010203u, 0xFFFFFFFFu, 0xC0A80109u);
	if (route_lookup(c, &nh, &fl, &oif) && memcmp(nh.bytes, c.bytes, 4) == 0 &&
	    !(fl & RTF_GATEWAY))
		console_write("M84-ROUTE: ok delete\n");
	else
		console_write("M84-ROUTE: FAIL delete\n");

	/* Metric tie-break: a second default route with a worse metric must not
	 * displace the installed one, and it must take over once the better one
	 * is deleted. */
	route_add_oif(0, 0, 0xC0A801FDu, RTF_UP, 100, 0);
	int metric_ok = route_lookup(d, &nh, &fl, &oif) && nh.bytes[3] == 254;
	route_del(0, 0, 0xC0A801FEu);
	metric_ok = metric_ok && route_lookup(d, &nh, &fl, &oif) &&
	            nh.bytes[3] == 253;
	console_write(metric_ok ? "M84-ROUTE: ok metric\n"
	                        : "M84-ROUTE: FAIL metric\n");

	/* ECMP: two equal-cost defaults share the traffic, but a given
	 * destination always picks the same next hop (no per-packet reordering).
	 * Over a spread of destinations both next hops must get used. */
	route_flush_all();
	route_add_oif(0, 0, 0xC0A80101u, RTF_UP, 0, 0);
	route_add_oif(0, 0, 0xC0A80102u, RTF_UP, 0, 0);
	int seen1 = 0, seen2 = 0, stable = 1;
	for (int i = 0; i < 64; i++) {
		struct ipv4_addr t = {{(u8)(11 + i), 1, 2, 3}};
		struct ipv4_addr first, again;
		if (!route_lookup(t, &first, &fl, &oif)) {
			stable = 0;
			break;
		}
		if (!route_lookup(t, &again, &fl, &oif) ||
		    memcmp(first.bytes, again.bytes, 4) != 0)
			stable = 0;
		if (first.bytes[3] == 1)
			seen1 = 1;
		else if (first.bytes[3] == 2)
			seen2 = 1;
	}
	console_write((stable && seen1 && seen2) ? "M84-ROUTE: ok ecmp\n"
	                                         : "M84-ROUTE: FAIL ecmp\n");

	/* Per-interface routes: the matched route reports which interface the
	 * packet must leave through. */
	route_flush_all();
	route_add_oif(0x0A000000u, 0xFF000000u, 0, RTF_UP, 0, 2);
	route_add_oif(0xC0A80000u, 0xFFFF0000u, 0, RTF_UP, 0, 1);
	int if_ok = route_lookup(a, &nh, &fl, &oif) && oif == 2;
	struct ipv4_addr e = {{192, 168, 5, 5}};
	if_ok = if_ok && route_lookup(e, &nh, &fl, &oif) && oif == 1;
	console_write(if_ok ? "M84-ROUTE: ok per-interface\n"
	                    : "M84-ROUTE: FAIL per-interface\n");
}

static void route_smoke_v6(void)
{
	struct in6_addr_k dst, gw, zero, nh;
	u16 fl;
	int oif;
	int ok = 1;

	memset(&zero, 0, sizeof(zero));

	/* The standing on-link routes (fe80::/10, ::1/128, ff02::/16) are part of
	 * what is under test, and route_smoke() flushed the live table — put them
	 * back exactly as boot does. */
	route6_init();

	/* 2001:db8::/32 via fe80::1, more specific 2001:db8:1::/48 on-link, and a
	 * /128 host route via fe80::9. */
	memset(&dst, 0, sizeof(dst));
	dst.bytes[0] = 0x20; dst.bytes[1] = 0x01;
	dst.bytes[2] = 0x0d; dst.bytes[3] = 0xb8;
	memset(&gw, 0, sizeof(gw));
	gw.bytes[0] = 0xfe; gw.bytes[1] = 0x80; gw.bytes[15] = 1;
	route6_add(dst, 32, gw, RTF_UP, 0, 0);

	dst.bytes[5] = 0x01;
	route6_add(dst, 48, zero, RTF_UP, 0, 0);

	struct in6_addr_k host = dst;
	host.bytes[15] = 0x33;
	struct in6_addr_k gw9 = gw;
	gw9.bytes[15] = 9;
	route6_add(host, 128, gw9, RTF_UP, 0, 0);

	/* 2001:db8:9:: matches only the /32 -> fe80::1 */
	struct in6_addr_k t = dst;
	t.bytes[5] = 0x09;
	t.bytes[15] = 0x77;
	if (!route6_lookup(t, &nh, &fl, &oif) || nh.bytes[15] != 1 ||
	    !(fl & RTF_GATEWAY))
		ok = 0;

	/* 2001:db8:1::44 matches the /48 -> on-link (next hop == destination) */
	struct in6_addr_k t2 = dst;
	t2.bytes[15] = 0x44;
	if (!route6_lookup(t2, &nh, &fl, &oif) ||
	    memcmp(nh.bytes, t2.bytes, 16) != 0 || (fl & RTF_GATEWAY))
		ok = 0;

	/* The /128 beats the /48. */
	if (!route6_lookup(host, &nh, &fl, &oif) || nh.bytes[15] != 9 ||
	    !(fl & RTF_HOST))
		ok = 0;

	console_write(ok ? "M84-ROUTE6: ok lpm\n" : "M84-ROUTE6: FAIL lpm\n");

	/* fe80::/10 is on-link without any router advertisement. */
	struct in6_addr_k ll;
	memset(&ll, 0, sizeof(ll));
	ll.bytes[0] = 0xfe; ll.bytes[1] = 0x80; ll.bytes[15] = 0x42;
	if (route6_lookup(ll, &nh, &fl, &oif) &&
	    memcmp(nh.bytes, ll.bytes, 16) == 0 && !(fl & RTF_GATEWAY))
		console_write("M84-ROUTE6: ok link-local-onlink\n");
	else
		console_write("M84-ROUTE6: FAIL link-local-onlink\n");

	/* Default route via a router, and deletion falling back to no route. */
	route6_add(zero, 0, gw, RTF_UP, 0, 0);
	struct in6_addr_k off;
	memset(&off, 0, sizeof(off));
	off.bytes[0] = 0x26; off.bytes[15] = 1;
	int def_ok = route6_lookup(off, &nh, &fl, &oif) && nh.bytes[15] == 1 &&
	             (fl & RTF_GATEWAY);
	route6_del(zero, 0, gw);
	def_ok = def_ok && !route6_lookup(off, &nh, &fl, &oif);
	console_write(def_ok ? "M84-ROUTE6: ok default\n"
	                     : "M84-ROUTE6: FAIL default\n");
}

/* Policy routing, flow-hashed ECMP and the text control plane. */
static void route_smoke_policy(void)
{
	struct ipv4_addr nh;
	u16 fl;
	int oif;

	route_flush_all();
	route_rules_reset();

	/* Two tables: the main one sends everything via .1, table 100 via .9.
	 * A rule binds 10.5.0.0/16 sources to table 100. */
	route_add_table(0, 0, 0xC0A80101u, RTF_UP, 0, 0, RT_TABLE_MAIN);
	route_add_table(0, 0, 0xC0A80109u, RTF_UP, 0, 0, 100);

	struct ipv4_addr src_a = {{10, 5, 1, 1}};
	struct ipv4_addr src_b = {{10, 6, 1, 1}};
	struct ipv4_addr mask16 = {{255, 255, 0, 0}};
	struct in6_addr_k z6;
	memset(&z6, 0, sizeof(z6));
	route_rule_add(4, 100, src_a, mask16, z6, 0, 0, 100);

	struct ipv4_addr dst = {{8, 8, 8, 8}};
	int ok = route_lookup_ex(src_a, dst, 0, 0, &nh, &fl, &oif) &&
	         nh.bytes[3] == 9;
	ok = ok && route_lookup_ex(src_b, dst, 0, 0, &nh, &fl, &oif) &&
	     nh.bytes[3] == 1;
	console_write(ok ? "M84-POLICY: ok source-rule\n"
	                 : "M84-POLICY: FAIL source-rule\n");

	/* Deleting the rule sends the same source back to the main table. */
	route_rule_del(4, 100);
	int back = route_lookup_ex(src_a, dst, 0, 0, &nh, &fl, &oif) &&
	           nh.bytes[3] == 1;
	console_write(back ? "M84-POLICY: ok rule-delete\n"
	                   : "M84-POLICY: FAIL rule-delete\n");

	/* A rule pointing at an empty table must fall through to the next rule
	 * rather than declaring the destination unreachable. */
	route_rule_add(4, 50, src_a, mask16, z6, 0, 0, 200 /* empty */);
	int fallthrough = route_lookup_ex(src_a, dst, 0, 0, &nh, &fl, &oif) &&
	                  nh.bytes[3] == 1;
	console_write(fallthrough ? "M84-POLICY: ok rule-fallthrough\n"
	                          : "M84-POLICY: FAIL rule-fallthrough\n");
	route_rule_del(4, 50);

	/* Flow-hashed ECMP: one destination, many flows, both next hops used, and
	 * each individual flow pinned to one of them. */
	route_flush_all();
	route_add_oif(0, 0, 0xC0A80101u, RTF_UP, 0, 0);
	route_add_oif(0, 0, 0xC0A80102u, RTF_UP, 0, 0);
	int seen1 = 0, seen2 = 0, stable = 1;
	struct ipv4_addr local = {{10, 0, 2, 15}};
	for (u16 sport = 1024; sport < 1088; sport++) {
		u32 flow = route_flow_hash(local.bytes, dst.bytes, 4, 6, sport, 80);
		struct ipv4_addr first, again;
		if (!route_lookup_flow(dst, flow, &first, &fl, &oif)) {
			stable = 0;
			break;
		}
		if (!route_lookup_flow(dst, flow, &again, &fl, &oif) ||
		    memcmp(first.bytes, again.bytes, 4) != 0)
			stable = 0;
		if (first.bytes[3] == 1)
			seen1 = 1;
		else if (first.bytes[3] == 2)
			seen2 = 1;
	}
	console_write((stable && seen1 && seen2)
	                  ? "M84-POLICY: ok ecmp-flow-hash\n"
	                  : "M84-POLICY: FAIL ecmp-flow-hash\n");

	/* More equal-cost hops than the old fixed candidate array held: the group
	 * width is bounded by the table, not by the lookup. */
	route_flush_all();
	for (u32 i = 1; i <= 6; i++)
		route_add_oif(0, 0, 0xC0A80100u + i, RTF_UP, 0, 0);
	u32 mask_used = 0;
	for (u16 sport = 1024; sport < 1600; sport++) {
		u32 flow = route_flow_hash(local.bytes, dst.bytes, 4, 6, sport, 80);
		if (route_lookup_flow(dst, flow, &nh, &fl, &oif))
			mask_used |= 1u << (nh.bytes[3] & 0x1F);
	}
	int wide = 1;
	for (u32 i = 1; i <= 6; i++) {
		if (!(mask_used & (1u << i)))
			wide = 0;
	}
	console_write(wide ? "M84-POLICY: ok ecmp-wide\n"
	                   : "M84-POLICY: FAIL ecmp-wide\n");

	/* The text control plane: the same commands userspace writes into
	 * /proc/net/rt_tables and /proc/net/rt_rules. */
	route_flush_all();
	route_rules_reset();
	const char *cmds =
	    "add 172.16.0.0/12 via 10.0.2.2 metric 5 table 42\n"
	    "add6 2001:db8:cafe::/48 via fe80::1 table 42\n"
	    "add default via 10.0.2.2\n";
	int ctl_ok = route_control_write(cmds, strlen(cmds)) == 0;
	const char *rules = "add 90 from 192.168.7.0/24 table 42\n";
	ctl_ok = ctl_ok && route_rule_control_write(rules, strlen(rules)) == 0;

	struct ipv4_addr psrc = {{192, 168, 7, 5}};
	struct ipv4_addr pdst = {{172, 16, 3, 4}};
	ctl_ok = ctl_ok && route_lookup_ex(psrc, pdst, 0, 0, &nh, &fl, &oif) &&
	         nh.bytes[3] == 2 && (fl & RTF_GATEWAY);

	struct in6_addr_k d6, nh6;
	memset(&d6, 0, sizeof(d6));
	d6.bytes[0] = 0x20; d6.bytes[1] = 0x01;
	d6.bytes[2] = 0x0d; d6.bytes[3] = 0xb8;
	d6.bytes[4] = 0xca; d6.bytes[5] = 0xfe;
	d6.bytes[15] = 0x99;
	/* The v6 rule set still points at main, so look the /48 up in table 42
	 * through a rule that selects it. */
	const char *rules6 = "add6 91 from any table 42\n";
	ctl_ok = ctl_ok && route_rule_control_write(rules6, strlen(rules6)) == 0;
	struct in6_addr_k any6;
	memset(&any6, 0, sizeof(any6));
	ctl_ok = ctl_ok &&
	         route6_lookup_ex(any6, d6, 0, 0, &nh6, &fl, &oif) &&
	         nh6.bytes[0] == 0xfe && nh6.bytes[15] == 1;

	/* A malformed command must be rejected, not partially applied. */
	const char *bad = "add 999.1.2.3/8 via 10.0.2.2\n";
	ctl_ok = ctl_ok && route_control_write(bad, strlen(bad)) != 0;

	console_write(ctl_ok ? "M84-POLICY: ok text-control\n"
	                     : "M84-POLICY: FAIL text-control\n");
}

/* The procfs plumbing itself: /proc/net/rt_rules and /proc/net/rt_tables must
 * exist, accept commands through a normal write(2), and show the result in a
 * normal read(2). This goes through the VFS the way userspace does. */
static void route_smoke_procfs(void)
{
	int ok = 1;

	int fd = vfs_open_flags("/proc/net/rt_rules", B1NIX_O_WRONLY);
	if (fd < 0) {
		console_write("M84-POLICY: FAIL procfs-open\n");
		return;
	}
	const char *cmd = "add 77 from 10.9.0.0/16 table 77\n";
	if (vfs_write(fd, cmd, strlen(cmd)) != (isize)strlen(cmd))
		ok = 0;
	vfs_close(fd);

	/* The rule must be live in the kernel table, not just accepted. */
	struct route_rule_info ru[16];
	usize n = route_rule_snapshot(ru, 16);
	int found = 0;
	for (usize i = 0; i < n; i++) {
		if (ru[i].family == 4 && ru[i].prio == 77 && ru[i].table == 77)
			found = 1;
	}
	ok = ok && found;

	/* ...and readable back out. */
	fd = vfs_open_flags("/proc/net/rt_rules", B1NIX_O_RDONLY);
	if (fd < 0) {
		ok = 0;
	} else {
		char buf[512];
		memset(buf, 0, sizeof(buf));
		isize got = vfs_read(fd, buf, sizeof(buf) - 1);
		vfs_close(fd);
		int saw = 0;
		for (isize i = 0; i + 2 < got; i++) {
			if (buf[i] == '7' && buf[i + 1] == '7' && buf[i + 2] == '\t')
				saw = 1;
		}
		ok = ok && got > 0 && saw;
	}

	/* A malformed command must be refused by write(2), not silently dropped. */
	fd = vfs_open_flags("/proc/net/rt_rules", B1NIX_O_WRONLY);
	if (fd >= 0) {
		const char *bad = "nonsense 1 2 3\n";
		if (vfs_write(fd, bad, strlen(bad)) >= 0)
			ok = 0;
		vfs_close(fd);
	}

	route_rule_del(4, 77);

	/* Same for the route table file. */
	fd = vfs_open_flags("/proc/net/rt_tables", B1NIX_O_WRONLY);
	if (fd < 0) {
		ok = 0;
	} else {
		const char *rt = "add 203.0.113.0/24 via 10.0.2.2 table 77\n";
		if (vfs_write(fd, rt, strlen(rt)) != (isize)strlen(rt))
			ok = 0;
		vfs_close(fd);
		struct route_info ri[64];
		usize rn = route_snapshot(ri, 64);
		int rfound = 0;
		for (usize i = 0; i < rn; i++) {
			if (ri[i].table == 77 && ri[i].dst == 0xCB007100u)
				rfound = 1;
		}
		ok = ok && rfound;
		route_del_table(0xCB007100u, 0xFFFFFF00u, 0x0A000202u, 77);
	}

	console_write(ok ? "M84-POLICY: ok procfs-control\n"
	                 : "M84-POLICY: FAIL procfs-control\n");
}

void route_smoke(void)
{
	struct route_info saved[ROUTE_MAX_ENTRIES];
	struct route6_info saved6[ROUTE6_MAX_ENTRIES];
	usize saved_n = route_snapshot(saved, ROUTE_MAX_ENTRIES);
	usize saved6_n = route6_snapshot(saved6, ROUTE6_MAX_ENTRIES);

	route_flush_all();
	route6_flush_all();

	route_smoke_v4();
	route_smoke_v6();
	route_smoke_policy();
	route_smoke_procfs();

	/* Restore the live tables and the default policy. */
	route_flush_all();
	route6_flush_all();
	route_rules_reset();
	for (usize i = 0; i < saved_n; i++)
		route_add_oif(saved[i].dst, saved[i].mask, saved[i].gateway,
		              saved[i].flags, saved[i].metric, saved[i].oif);
	for (usize i = 0; i < saved6_n; i++) {
		struct in6_addr_k d, g;
		memcpy(d.bytes, saved6[i].dst, 16);
		memcpy(g.bytes, saved6[i].gateway, 16);
		route6_add(d, saved6[i].plen, g, saved6[i].flags, saved6[i].metric,
		           saved6[i].oif);
	}
	route_configure_interface(net_get_ip(), net_get_netmask(),
	                          net_get_gateway());
}

/* ── Text control interface (/proc/net/rt_tables, /proc/net/rt_rules) ──────
 * Policy routing needs an administrative interface, and b1nix has no rtnetlink
 * to carry RTM_NEWRULE. These two procfs files are that interface: one line
 * per command, the same vocabulary `ip route` / `ip rule` use.
 *
 *   /proc/net/rt_tables:
 *     add  <a.b.c.d/len> [via <a.b.c.d>] [dev ethN] [metric M] [table T]
 *     add6 <ipv6/len>    [via <ipv6>]    [dev ethN] [metric M] [table T]
 *     del  <a.b.c.d/len> [via <a.b.c.d>] [table T]
 *     del6 <ipv6/len>    [via <ipv6>]    [table T]
 *     flush [table T]
 *
 *   /proc/net/rt_rules:
 *     add  <prio> [from <a.b.c.d/len>] [iif ethN] table <T>
 *     add6 <prio> [from <ipv6/len>]    [iif ethN] table <T>
 *     del  <prio> | del6 <prio>
 *     reset
 */

static int rt_isspace(char c) { return c == ' ' || c == '\t'; }

static const char *rt_skip_ws(const char *p, const char *end)
{
	while (p < end && rt_isspace(*p))
		p++;
	return p;
}

/* Copy the next whitespace-delimited token into out[]; returns the position
 * after it, or NULL when the input is exhausted. */
static const char *rt_token(const char *p, const char *end, char *out,
                            usize cap)
{
	p = rt_skip_ws(p, end);
	if (p >= end || *p == '\n')
		return 0;
	usize n = 0;
	while (p < end && !rt_isspace(*p) && *p != '\n') {
		if (n + 1 < cap)
			out[n++] = *p;
		p++;
	}
	out[n] = '\0';
	return p;
}

static int rt_parse_u32(const char *s, u32 *out)
{
	u32 v = 0;
	int any = 0;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (u32)(*s - '0');
		s++;
		any = 1;
	}
	if (!any || *s != '\0')
		return -1;
	*out = v;
	return 0;
}

/* "a.b.c.d" or "a.b.c.d/len". A prefix length of -1 means none was given. */
static int rt_parse_ipv4(const char *s, struct ipv4_addr *out, int *plen)
{
	u32 octet = 0;
	int digits = 0;
	int idx = 0;
	u8 b[4] = {0, 0, 0, 0};

	if (plen)
		*plen = -1;
	for (;; s++) {
		if (*s >= '0' && *s <= '9') {
			octet = octet * 10 + (u32)(*s - '0');
			if (octet > 255)
				return -1;
			digits++;
		} else if (*s == '.' || *s == '\0' || *s == '/') {
			if (!digits || idx > 3)
				return -1;
			b[idx++] = (u8)octet;
			octet = 0;
			digits = 0;
			if (*s == '\0' || *s == '/')
				break;
		} else {
			return -1;
		}
	}
	if (idx != 4)
		return -1;
	memcpy(out->bytes, b, 4);
	if (*s == '/') {
		u32 v;
		if (rt_parse_u32(s + 1, &v) != 0 || v > 32)
			return -1;
		if (plen)
			*plen = (int)v;
	}
	return 0;
}

static int rt_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* RFC 4291 text form, including "::" compression, optionally "/len". */
static int rt_parse_ipv6(const char *s, struct in6_addr_k *out, int *plen)
{
	u8 head[16], tail[16];
	int nhead = 0, ntail = 0;
	int after_dcolon = 0;

	if (plen)
		*plen = -1;
	memset(head, 0, sizeof(head));
	memset(tail, 0, sizeof(tail));

	if (s[0] == ':' && s[1] == ':') {
		after_dcolon = 1;
		s += 2;
	} else if (s[0] == ':') {
		return -1;
	}

	while (*s && *s != '/') {
		int d, v = 0, digits = 0;
		while ((d = rt_hexval(*s)) >= 0) {
			v = (v << 4) | d;
			if (v > 0xFFFF)
				return -1;
			s++;
			digits++;
		}
		if (!digits)
			return -1;
		u8 *dstbuf = after_dcolon ? tail : head;
		int *cnt = after_dcolon ? &ntail : &nhead;
		if (*cnt + 2 > 16)
			return -1;
		dstbuf[*cnt] = (u8)(v >> 8);
		dstbuf[*cnt + 1] = (u8)v;
		*cnt += 2;

		if (*s == ':') {
			s++;
			if (*s == ':') {
				if (after_dcolon)
					return -1;
				after_dcolon = 1;
				s++;
			}
		} else if (*s != '\0' && *s != '/') {
			return -1;
		}
	}

	if (!after_dcolon && nhead != 16)
		return -1;
	if (nhead + ntail > 16)
		return -1;

	memset(out->bytes, 0, 16);
	memcpy(out->bytes, head, (usize)nhead);
	if (ntail)
		memcpy(out->bytes + 16 - ntail, tail, (usize)ntail);

	if (*s == '/') {
		u32 v;
		if (rt_parse_u32(s + 1, &v) != 0 || v > 128)
			return -1;
		if (plen)
			*plen = (int)v;
	}
	return 0;
}

static u32 rt_mask_from_plen(int plen)
{
	if (plen <= 0)
		return 0;
	if (plen >= 32)
		return 0xFFFFFFFFu;
	return 0xFFFFFFFFu << (32 - plen);
}

/* One line of /proc/net/rt_tables. Returns 0 or a negative errno-ish code. */
static int route_ctl_line(const char *p, const char *end)
{
	char tok[64];
	const char *q = rt_token(p, end, tok, sizeof(tok));
	if (!q)
		return 0; /* blank line */

	int v6 = 0;
	int del = 0;
	if (tok[0] == '#')
		return 0;
	if (!strcmp(tok, "add")) {
		/* nothing */
	} else if (!strcmp(tok, "add6")) {
		v6 = 1;
	} else if (!strcmp(tok, "del")) {
		del = 1;
	} else if (!strcmp(tok, "del6")) {
		del = 1;
		v6 = 1;
	} else if (!strcmp(tok, "flush")) {
		u32 table = RT_TABLE_MAIN;
		const char *r = rt_token(q, end, tok, sizeof(tok));
		if (r && !strcmp(tok, "table")) {
			r = rt_token(r, end, tok, sizeof(tok));
			if (!r || rt_parse_u32(tok, &table) != 0)
				return -1;
		}
		u64 f;
		spin_lock_irqsave(&route_lock, &f);
		for (int i = 0; i < ROUTE_MAX_ENTRIES; i++) {
			if (routes[i].used && routes[i].table == table)
				memset(&routes[i], 0, sizeof(routes[i]));
		}
		for (int i = 0; i < ROUTE6_MAX_ENTRIES; i++) {
			if (routes6[i].used && routes6[i].table == table)
				memset(&routes6[i], 0, sizeof(routes6[i]));
		}
		spin_unlock_irqrestore(&route_lock, f);
		return 0;
	} else {
		return -1;
	}

	struct ipv4_addr d4 = {{0, 0, 0, 0}}, g4 = {{0, 0, 0, 0}};
	struct in6_addr_k d6, g6;
	int plen = -1;
	memset(&d6, 0, sizeof(d6));
	memset(&g6, 0, sizeof(g6));

	q = rt_token(q, end, tok, sizeof(tok));
	if (!q)
		return -1;
	if (!strcmp(tok, "default")) {
		plen = 0;
	} else if (v6) {
		if (rt_parse_ipv6(tok, &d6, &plen) != 0)
			return -1;
		if (plen < 0)
			plen = 128;
	} else {
		if (rt_parse_ipv4(tok, &d4, &plen) != 0)
			return -1;
		if (plen < 0)
			plen = 32;
	}

	u32 table = RT_TABLE_MAIN;
	u32 metric = 0;
	int oif = 0;
	int have_gw = 0;
	for (;;) {
		const char *r = rt_token(q, end, tok, sizeof(tok));
		if (!r)
			break;
		q = r;
		if (!strcmp(tok, "via")) {
			q = rt_token(q, end, tok, sizeof(tok));
			if (!q)
				return -1;
			if (v6) {
				if (rt_parse_ipv6(tok, &g6, 0) != 0)
					return -1;
			} else if (rt_parse_ipv4(tok, &g4, 0) != 0) {
				return -1;
			}
			have_gw = 1;
		} else if (!strcmp(tok, "dev")) {
			q = rt_token(q, end, tok, sizeof(tok));
			if (!q)
				return -1;
			oif = netdev_index_by_name(tok);
		} else if (!strcmp(tok, "metric")) {
			q = rt_token(q, end, tok, sizeof(tok));
			if (!q || rt_parse_u32(tok, &metric) != 0)
				return -1;
		} else if (!strcmp(tok, "table")) {
			q = rt_token(q, end, tok, sizeof(tok));
			if (!q || rt_parse_u32(tok, &table) != 0)
				return -1;
		} else {
			return -1;
		}
	}
	(void)have_gw;

	if (v6) {
		if (del)
			return route6_del_table(d6, (u8)plen, g6, table);
		return route6_add_table(d6, (u8)plen, g6, RTF_UP, (u16)metric, oif,
		                        table);
	}
	u32 mask = rt_mask_from_plen(plen);
	if (del)
		return route_del_table(route_ipv4_to_host(d4), mask,
		                       route_ipv4_to_host(g4), table);
	return route_add_table(route_ipv4_to_host(d4), mask,
	                       route_ipv4_to_host(g4), RTF_UP, (u16)metric, oif,
	                       table);
}

static int route_rule_ctl_line(const char *p, const char *end)
{
	char tok[64];
	const char *q = rt_token(p, end, tok, sizeof(tok));
	if (!q)
		return 0;
	if (tok[0] == '#')
		return 0;

	if (!strcmp(tok, "reset")) {
		route_rules_reset();
		return 0;
	}

	int v6 = 0, del = 0;
	if (!strcmp(tok, "add")) {
	} else if (!strcmp(tok, "add6")) {
		v6 = 1;
	} else if (!strcmp(tok, "del")) {
		del = 1;
	} else if (!strcmp(tok, "del6")) {
		del = 1;
		v6 = 1;
	} else {
		return -1;
	}

	u32 prio = 0;
	q = rt_token(q, end, tok, sizeof(tok));
	if (!q || rt_parse_u32(tok, &prio) != 0)
		return -1;
	if (del)
		return route_rule_del(v6 ? 6 : 4, prio);

	struct ipv4_addr src4 = {{0, 0, 0, 0}}, mask4 = {{0, 0, 0, 0}};
	struct in6_addr_k src6;
	memset(&src6, 0, sizeof(src6));
	u8 plen6 = 0;
	int iif = 0;
	u32 table = RT_TABLE_MAIN;

	for (;;) {
		const char *r = rt_token(q, end, tok, sizeof(tok));
		if (!r)
			break;
		q = r;
		if (!strcmp(tok, "from")) {
			q = rt_token(q, end, tok, sizeof(tok));
			if (!q)
				return -1;
			if (!strcmp(tok, "all") || !strcmp(tok, "any"))
				continue;
			int plen = -1;
			if (v6) {
				if (rt_parse_ipv6(tok, &src6, &plen) != 0)
					return -1;
				plen6 = (u8)(plen < 0 ? 128 : plen);
			} else {
				if (rt_parse_ipv4(tok, &src4, &plen) != 0)
					return -1;
				mask4 = route_host_to_ipv4(rt_mask_from_plen(plen < 0 ? 32
				                                                      : plen));
			}
		} else if (!strcmp(tok, "iif") || !strcmp(tok, "dev")) {
			q = rt_token(q, end, tok, sizeof(tok));
			if (!q)
				return -1;
			iif = netdev_index_by_name(tok);
		} else if (!strcmp(tok, "table")) {
			q = rt_token(q, end, tok, sizeof(tok));
			if (!q || rt_parse_u32(tok, &table) != 0)
				return -1;
		} else {
			return -1;
		}
	}

	return route_rule_add(v6 ? 6 : 4, prio, src4, mask4, src6, plen6, iif,
	                      table);
}

/* Apply every newline-separated command in the buffer. Returns 0 if all of
 * them applied, -1 on the first malformed or failing line. */
static int route_ctl_apply(const char *buf, usize len,
                           int (*fn)(const char *, const char *))
{
	const char *p = buf;
	const char *end = buf + len;
	while (p < end) {
		const char *nl = p;
		while (nl < end && *nl != '\n')
			nl++;
		if (fn(p, nl) != 0)
			return -1;
		p = (nl < end) ? nl + 1 : end;
	}
	return 0;
}

int route_control_write(const char *buf, usize len)
{
	return route_ctl_apply(buf, len, route_ctl_line);
}

int route_rule_control_write(const char *buf, usize len)
{
	return route_ctl_apply(buf, len, route_rule_ctl_line);
}
