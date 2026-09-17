#include <stdio.h>
#include <b1nix/kprintf.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/namespace.h>
#include <b1nix/net.h>
#include <b1nix/netproto.h>
#include <b1nix/netdev.h>
#include <b1nix/netlink.h>
#include <b1nix/packet.h>
#include <b1nix/pci.h>
#include <b1nix/ipi.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <string.h>

/*
 * Driver-agnostic network glue. The actual NIC drivers live in kernel/dev/
 * (virtio_net.c, e1000.c) and register a struct netdev here; this file owns the
 * interface address state (MAC/IPv4/IPv6), the ethernet TX entry point, the
 * net_poll() pump, the loopback datapath, and the net_task daemon.
 */

/* ── Active NIC registry ────────────────────────────────────────────────── */
/* A veth pair costs two slots and a namespace usually gets one pair on top of
 * whatever the initial namespace already holds; a deleted device's slot is not
 * reused (see netdev_unregister). The ceiling sits just under
 * NETLINK_LO_IFINDEX, the index loopback is presented under. */
#define NET_MAX_NETDEVS (NETLINK_LO_IFINDEX - 1)

static struct netdev *netdev_best(void);
static void net_reset_interface_state(struct netdev *nd);

static struct netdev *g_netdev;
static struct netdev *g_receiving_netdev;
static struct netdev *g_netdevs[NET_MAX_NETDEVS];
static usize g_netdev_count;

/* Next free eth<N> for a driver that did not name its device. Virtual devices
 * arrive with a name of their own and never take one of these. */
static void netdev_assign_ethname(struct netdev *nd)
{
	for (int n = 0; n < 10; n++) {
		char candidate[8] = { 'e', 't', 'h', (char)('0' + n), '\0', 0, 0, 0 };
		int taken = 0;
		for (usize i = 0; i < g_netdev_count; i++) {
			if (g_netdevs[i] && strcmp(g_netdevs[i]->ifname, candidate) == 0)
				taken = 1;
		}
		if (!taken) {
			memcpy(nd->ifname, candidate, sizeof(candidate));
			return;
		}
	}
	nd->ifname[0] = '\0';
}

void netdev_register(struct netdev *nd)
{
	if (!nd)
		return;
	for (usize i = 0; i < g_netdev_count; i++) {
		if (g_netdevs[i] == nd)
			return;
	}
	if (nd->ifname[0] == '\0')
		netdev_assign_ethname(nd);
	/* An interface is born in the namespace of whoever created it. A driver
	 * probing at boot is in the initial namespace, so this is 0 for every NIC;
	 * a device created by `ip link add` inside a namespace belongs to it. */
	nd->netns = namespace_net_current();
	if (g_netdev_count < NET_MAX_NETDEVS)
		g_netdevs[g_netdev_count++] = nd;
	if (!g_netdev && !netdev_is_virtual(nd) && nd->netns == 0)
		g_netdev = nd;
}

/* The namespace an interface lookup should be answered in. Inside a receive
 * path that is the arriving interface's namespace; otherwise it is the calling
 * task's. Both are 0 until something unshares, and namespace_net_context()
 * short-circuits on that. */
static u32 net_ns_ctx(void) { return namespace_net_context(); }

struct netdev *netdev_slot(int idx)
{
	if (idx <= 0 || (usize)idx > g_netdev_count)
		return 0;
	return g_netdevs[idx - 1];
}

usize netdev_slot_count(void) { return g_netdev_count; }

int netdev_set_netns(struct netdev *nd, u32 ns)
{
	if (!nd)
		return -ENODEV;
	if (!namespace_net_live(ns))
		return -EINVAL;
	if (nd->netns == ns)
		return 0;
	/* Moving the interface the initial namespace routes through would strand
	 * every socket already using it. Linux allows it; b1nix has exactly one
	 * L3 configuration, so it would be a one-way trip to no networking. */
	if (nd == g_netdev)
		return -EBUSY;
	/* Stacking does not survive the move: a bridge port, a VLAN's lower
	 * device or a bond slave that lands in another namespace would forward
	 * frames across the boundary, which is the one thing the boundary is for.
	 * The veth pair is the sanctioned way through. */
	if (nd->master || nd->lower)
		return -EBUSY;
	for (usize i = 0; i < g_netdev_count; i++) {
		if (!g_netdevs[i])
			continue;
		if (g_netdevs[i]->master == nd || g_netdevs[i]->lower == nd)
			return -EBUSY;
	}
	u32 from = nd->netns;
	nd->netns = ns;
	/* A namespace that has just lost its last interface has nothing left to
	 * hold an address: keeping the L3 configuration would leave it answering
	 * for an address no cable reaches. The initial namespace is handled by
	 * net_reset_interface_state() instead, which knows about DHCP. */
	if (from != 0 && !netdev_active_ns(from)) {
		net_ns_clear_ipv4(from);
		net_ns_clear_ipv6(from);
	}
	return 0;
}

/* Tear a network namespace down: everything created in it goes with it, and a
 * physical NIC that was moved into it returns to the initial namespace rather
 * than becoming unreachable (this is what Linux does too). */
/* net.ipv4.ping_group_range, per network namespace: the kernel group ids
 * allowed to open ICMP datagram ("ping") sockets. Linux's default is the empty
 * range "1 0"; `set` says a namespace was given another. */
static struct {
	u32 lo, hi;
	u8 set;
} g_ping_group_range[NS_MAX_NET];

void net_ping_group_range(u32 ns, u32 *lo, u32 *hi)
{
	if (ns < NS_MAX_NET && g_ping_group_range[ns].set) {
		*lo = g_ping_group_range[ns].lo;
		*hi = g_ping_group_range[ns].hi;
	} else {
		*lo = 1;
		*hi = 0;
	}
}

void net_ping_group_range_set(u32 ns, u32 lo, u32 hi)
{
	if (ns >= NS_MAX_NET)
		return;
	g_ping_group_range[ns].lo = lo;
	g_ping_group_range[ns].hi = hi;
	g_ping_group_range[ns].set = 1;
}

void net_ns_destroy(u32 ns)
{
	if (ns == 0)
		return;
	if (ns < NS_MAX_NET)
		g_ping_group_range[ns].set = 0;
	for (usize i = 0; i < g_netdev_count; i++) {
		struct netdev *nd = g_netdevs[i];
		if (!nd || nd->netns != ns)
			continue;
		if (netdev_is_virtual(nd) && nd->destroy) {
			nd->destroy(nd);
			continue;
		}
		nd->netns = 0;
	}
	net_ns_clear_ipv4(ns);
	net_ns_clear_ipv6(ns);
	route_flush_ns(ns);
	arp_flush_ns(ns);
}

int netdev_is_virtual(const struct netdev *nd)
{
	return nd && nd->kind != NETDEV_KIND_PHYS;
}

void netdev_unregister(struct netdev *nd)
{
	if (!nd)
		return;
	for (usize i = 0; i < g_netdev_count; i++) {
		if (g_netdevs[i] != nd)
			continue;
		/* The slot is emptied, not compacted: the index is this interface's
		 * identity for as long as anything remembers it. */
		g_netdevs[i] = 0;
		break;
	}
	/* Anything stacked on the departing device loses its footing. */
	for (usize i = 0; i < g_netdev_count; i++) {
		if (!g_netdevs[i])
			continue;
		if (g_netdevs[i]->master == nd)
			g_netdevs[i]->master = 0;
		if (g_netdevs[i]->lower == nd)
			g_netdevs[i]->lower = 0;
	}
	if (g_netdev == nd) {
		dhcp_stop();
		struct netdev *next = netdev_best();
		g_netdev = next;
		net_reset_interface_state(next);
	}
	if (g_receiving_netdev == nd)
		g_receiving_netdev = 0;
}

/* The device a namespace routes through — the one its IPv4 configuration
 * belongs to.
 *
 * The initial namespace has g_netdev, chosen by carrier and maintained by the
 * DHCP/failover machinery. A namespace created by unshare(CLONE_NEWNET) has
 * none of that: it holds whatever interfaces were moved into it, so its active
 * device is simply the first administratively up one. Virtual devices count
 * there — a veth end is normally the ONLY interface such a namespace has, and
 * refusing it would leave the namespace unable to hold an address at all. */
struct netdev *netdev_active_ns(u32 ns)
{
	if (ns == 0)
		return g_netdev;
	for (usize i = 0; i < g_netdev_count; i++) {
		struct netdev *nd = g_netdevs[i];
		if (!nd || nd->netns != ns || nd->admin_down)
			continue;
		return nd;
	}
	return 0;
}

struct netdev *netdev_active(void)
{
	return netdev_active_ns(net_ns_ctx());
}

/* Is this the interface its namespace's IPv4 configuration is attached to?
 *
 * "Active" alone is not the question a destroy or an enslave has to ask. A
 * namespace's only veth end is active the moment it comes up, addressed or
 * not, and refusing to delete an unaddressed cable would be a rule invented
 * by the implementation. What must not happen silently is an interface
 * carrying an address being taken away from under the sockets using it. */
int netdev_holds_address(struct netdev *nd)
{
	if (!nd || nd != netdev_active_ns(nd->netns))
		return 0;
	struct net_v4_addr_info one;
	return net_ipv4_addr_list(nd->netns, &one, 1) != 0;
}
struct netdev *netdev_receiving(void) { return g_receiving_netdev; }

/* M84: interface indices. Registration order defines a stable 1-based index
 * (0 means "unspecified" in the FIB — route out of whatever is active), and
 * index N is presented to userspace as eth<N-1>, matching what ifconfig and
 * /proc/net/route show. */
int netdev_index_of(struct netdev *nd)
{
	if (!nd)
		return 0;
	for (usize i = 0; i < g_netdev_count; i++) {
		if (g_netdevs[i] == nd)
			return (int)i + 1;
	}
	return 0;
}

struct netdev *netdev_by_index(int idx)
{
	if (idx <= 0 || (usize)idx > g_netdev_count)
		return 0;
	struct netdev *nd = g_netdevs[idx - 1]; /* NULL for a retired index */
	/* An index belonging to another namespace resolves to nothing, so every
	 * caller that turns an ifindex into a device — the ioctls, the netlink
	 * dumps, the FIB's oif — is namespace-scoped by construction. */
	if (nd && nd->netns != net_ns_ctx())
		return 0;
	return nd;
}

void netdev_ifname(int idx, char *out, usize cap)
{
	if (!out || cap < 6)
		return;
	struct netdev *nd = netdev_by_index(idx);
	if (nd && nd->ifname[0]) {
		usize n = strlen(nd->ifname);
		if (n > cap - 1)
			n = cap - 1;
		memcpy(out, nd->ifname, n);
		out[n] = '\0';
		return;
	}
	/* No such device: fall back to the positional name so callers that ask
	 * about an index they have not checked still get something printable. */
	int n = idx > 0 ? idx - 1 : 0;
	if (n > 9)
		n = 9;
	out[0] = 'e';
	out[1] = 't';
	out[2] = 'h';
	out[3] = (char)('0' + n);
	out[4] = '\0';
}

int netdev_index_by_name(const char *name)
{
	if (!name || !name[0])
		return 0;
	u32 ns = net_ns_ctx();
	for (usize i = 0; i < g_netdev_count; i++) {
		if (g_netdevs[i] && g_netdevs[i]->netns == ns &&
		    strcmp(g_netdevs[i]->ifname, name) == 0)
			return (int)i + 1;
	}
	return 0;
}

static int netdev_link_state(struct netdev *nd)
{
	if (!nd)
		return 0;
	return nd->link_up ? nd->link_up(nd) : -1;
}

static struct netdev *netdev_best(void)
{
	/* An administratively down interface is not a candidate, whatever its
	 * carrier says — that is the whole point of taking it down. Only the
	 * initial namespace's devices are candidates: g_netdev carries the one L3
	 * configuration this kernel has. */
	for (usize i = 0; i < g_netdev_count; i++) {
		if (!g_netdevs[i] || netdev_is_virtual(g_netdevs[i]))
			continue;
		if (g_netdevs[i]->netns != 0)
			continue;
		if (!g_netdevs[i]->admin_down &&
		    netdev_link_state(g_netdevs[i]) == 1)
			return g_netdevs[i];
	}
	for (usize i = 0; i < g_netdev_count; i++) {
		if (!g_netdevs[i] || netdev_is_virtual(g_netdevs[i]))
			continue;
		if (g_netdevs[i]->netns != 0)
			continue;
		if (!g_netdevs[i]->admin_down)
			return g_netdevs[i];
	}
	return 0;
}

int netdev_is_admin_up(const struct netdev *nd)
{
	return nd ? !nd->admin_down : 0;
}

/* ── Interface address state ────────────────────────────────────────────── */
static volatile int net_irq_pending = 0;
static volatile int net_task_id = -1;

/* Why does the poll interval keep going back to one tick?
 *
 * Two explanations were tried and both were wrong -- traffic resetting the
 * backoff (disproved: 1,679 of 1,688 wakes were this task's own timer finding
 * nothing) and an imprecise reset test (replaced with the exact one; the
 * numbers did not move). So stop guessing and count the causes. Under
 * b1nix.waitprof only. */
static u64 g_beat_early, g_beat_irq, g_beat_lb, g_beat_calm;
static u64 g_beat_level[8]; /* how often each backoff level was in force */

static void net_beat_count(int early, int irq, int lb, u64 level)
{
	if (early)
		g_beat_early++;
	if (irq)
		g_beat_irq++;
	if (lb)
		g_beat_lb++;
	if (!early && !irq && !lb)
		g_beat_calm++;
	for (unsigned i = 0; i < 8; i++) {
		if (level <= (1ull << i) || i == 7) {
			g_beat_level[i]++;
			break;
		}
	}
}

void net_beat_dump(void)
{
	if (!bootinfo_has_flag("b1nix.waitprof"))
		return;
	console_write("net-beat: early ");
	console_write_dec(g_beat_early);
	console_write(" irq ");
	console_write_dec(g_beat_irq);
	console_write(" loopback ");
	console_write_dec(g_beat_lb);
	console_write(" calm ");
	console_write_dec(g_beat_calm);
	console_write(" | levels");
	for (unsigned i = 0; i < 8; i++) {
		console_write(" ");
		console_write_dec(g_beat_level[i]);
	}
	console_write("\n");
}

/* Is a loopback packet queued? Defined with the queue further down. */
static int net_loopback_pending(void);

/* How long net_task may sleep when there is no traffic, in scheduler ticks.
 * `b1nix.net-idle-ticks=N`, default 10; 1 restores the old fixed 100 Hz beat. */
static u64 net_idle_tick_cap(void)
{
	static u64 cap = ~0ull;

	if (cap == ~0ull)
		cap = bootinfo_get_u32("b1nix.net-idle-ticks", 10);
	return cap ? cap : 1;
}
static int networking_enabled;
static int last_link_state = -2;

static struct mac_addr local_mac;

/* The IPv4 configuration, one set per network namespace.
 *
 * This used to be three file-scope globals, which is why an interface moved
 * into a namespace could carry frames but never an address: every reader — the
 * transmit path's source stamp, the receive path's "is this for us", ARP, the
 * ioctls, netlink — read the single configuration the initial namespace owned.
 * Indexing by namespace makes each of those readers answer in its own
 * namespace with no other change, and slot 0 is the initial namespace, so its
 * behaviour is exactly what it was.
 *
 * M84: the netmask is real state (DHCP option 1), not a /24 assumption baked
 * into ipv4_send/procfs. It defines the on-link prefix the FIB installs. */
/* Addresses beyond the primary one: `ip addr add` of a second address, or an
 * ifconfig alias. Each carries its own prefix, so each has its own on-link
 * route and is the source of whatever is sent onto that prefix. */
struct net_v4_secondary {
	int used;
	struct ipv4_addr ip;
	struct ipv4_addr mask;
	char label[16];
};

struct net_ns_ipv4 {
	struct ipv4_addr ip;
	struct ipv4_addr gateway;
	struct ipv4_addr netmask;
	struct net_v4_secondary sec[NET_V4_MAX_ADDRS - 1];
};

static struct net_ns_ipv4 net_ns_v4[NS_MAX_NET];

static struct net_ns_ipv4 *net_v4(u32 ns)
{
	/* An out-of-range id can only come from a caller that invented one; the
	 * initial namespace is the safe answer and never a silent write into
	 * another namespace's state. */
	return &net_ns_v4[ns < NS_MAX_NET ? ns : 0];
}

/* IPv6 interface state, one set per network namespace. The initial
 * namespace's link-local is derived from the active NIC's MAC when that NIC is
 * chosen; any other namespace's comes from the MAC of the interface moved into
 * it. The global address / prefix / gateway are filled in by SLAAC (ndp.c),
 * and `ip -6 addr add` assigns further addresses next to them. */
struct net_ns_ipv6 {
	struct in6_addr_k global;   /* SLAAC / DHCPv6, 0 until set */
	struct in6_addr_k gateway;  /* router link-local */
	struct in6_addr_k prefix;   /* on-link /64 prefix */
	int prefix_valid;
	struct {
		int used;
		struct in6_addr_k addr;
		u8 plen;
	} assigned[NET_V6_MAX_ADDRS];
};

static struct net_ns_ipv6 net_ns_v6[NS_MAX_NET];
static struct in6_addr_k local_ip6_ll; /* the initial namespace's fe80::/64 */

static struct net_ns_ipv6 *net_v6(u32 ns)
{
	return &net_ns_v6[ns < NS_MAX_NET ? ns : 0];
}

#define NET_MAX_ADAPTERS 8

struct net_adapter {
	struct pci_device_info pci;
	u32 bars[6];
};

static struct net_adapter net_adapters[NET_MAX_ADAPTERS];
static usize net_adapter_count;

/* The station address of the interface the caller's namespace transmits
 * through. Only the initial namespace caches one (local_mac follows whichever
 * NIC is active); anywhere else it is the moved-in interface's own. */
struct mac_addr net_get_mac(void)
{
	u32 ns = net_ns_ctx();
	if (ns == 0)
		return local_mac;
	struct netdev *nd = netdev_active_ns(ns);
	return nd ? nd->mac : (struct mac_addr){{0, 0, 0, 0, 0, 0}};
}

struct ipv4_addr net_get_ip_ns(u32 ns) { return net_v4(ns)->ip; }
struct ipv4_addr net_get_gateway_ns(u32 ns) { return net_v4(ns)->gateway; }
struct ipv4_addr net_get_netmask_ns(u32 ns) { return net_v4(ns)->netmask; }
void net_set_ip_ns(u32 ns, struct ipv4_addr ip) { net_v4(ns)->ip = ip; }
void net_set_gateway_ns(u32 ns, struct ipv4_addr gw) { net_v4(ns)->gateway = gw; }
void net_set_netmask_ns(u32 ns, struct ipv4_addr m) { net_v4(ns)->netmask = m; }

void net_ns_clear_ipv4(u32 ns)
{
	memset(net_v4(ns), 0, sizeof(struct net_ns_ipv4));
}

struct ipv4_addr net_get_ip(void) { return net_get_ip_ns(net_ns_ctx()); }
struct ipv4_addr net_get_gateway(void) { return net_get_gateway_ns(net_ns_ctx()); }
struct ipv4_addr net_get_netmask(void) { return net_get_netmask_ns(net_ns_ctx()); }
void net_set_ip(struct ipv4_addr ip) { net_set_ip_ns(net_ns_ctx(), ip); }
void net_set_gateway(struct ipv4_addr gw) { net_set_gateway_ns(net_ns_ctx(), gw); }
void net_set_netmask(struct ipv4_addr m) { net_set_netmask_ns(net_ns_ctx(), m); }

/* ── Every IPv4 address of a namespace ─────────────────────────────────── */

static int ip4_is_zero(struct ipv4_addr a)
{
	return (a.bytes[0] | a.bytes[1] | a.bytes[2] | a.bytes[3]) == 0;
}

static int ip4_eq(struct ipv4_addr a, struct ipv4_addr b)
{
	return memcmp(a.bytes, b.bytes, 4) == 0;
}

static int ip4_same_prefix(struct ipv4_addr a, struct ipv4_addr b,
                           struct ipv4_addr mask)
{
	if (ip4_is_zero(mask))
		return 0;
	for (int i = 0; i < 4; i++)
		if ((a.bytes[i] & mask.bytes[i]) != (b.bytes[i] & mask.bytes[i]))
			return 0;
	return 1;
}

int net_ipv4_addr_add(u32 ns, struct ipv4_addr ip, struct ipv4_addr mask,
                      const char *label)
{
	struct net_ns_ipv4 *v = net_v4(ns);
	int is_alias = label && label[0];

	if (ip4_is_zero(ip))
		return -EINVAL;
	/* The primary address again: a new prefix length for it. */
	if (!is_alias && ip4_eq(v->ip, ip)) {
		if (!ip4_is_zero(mask))
			v->netmask = mask;
		return 0;
	}
	struct net_v4_secondary *free_slot = 0;
	for (usize i = 0; i < NET_V4_MAX_ADDRS - 1; i++) {
		struct net_v4_secondary *s = &v->sec[i];
		if (!s->used) {
			if (!free_slot)
				free_slot = s;
			continue;
		}
		if (ip4_eq(s->ip, ip) || (is_alias && strcmp(s->label, label) == 0)) {
			/* Re-adding an address updates it; re-addressing an alias moves
			 * the alias. */
			s->ip = ip;
			if (!ip4_is_zero(mask))
				s->mask = mask;
			if (is_alias)
				strncpy(s->label, label, sizeof(s->label) - 1);
			return 0;
		}
	}
	/* The first address an interface is given is its primary, unless it is
	 * explicitly an alias. */
	if (!is_alias && ip4_is_zero(v->ip)) {
		v->ip = ip;
		v->netmask = mask;
		return 0;
	}
	if (!free_slot)
		return -ENOSPC;
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = 1;
	free_slot->ip = ip;
	free_slot->mask = mask;
	if (is_alias)
		strncpy(free_slot->label, label, sizeof(free_slot->label) - 1);
	return 0;
}

int net_ipv4_addr_del(u32 ns, struct ipv4_addr ip)
{
	struct net_ns_ipv4 *v = net_v4(ns);

	if (ip4_is_zero(ip))
		return -EADDRNOTAVAIL;
	if (ip4_eq(v->ip, ip)) {
		/* Deleting the primary promotes the next address rather than
		 * silently dropping every other address with it. */
		memset(&v->ip, 0, sizeof(v->ip));
		memset(&v->netmask, 0, sizeof(v->netmask));
		for (usize i = 0; i < NET_V4_MAX_ADDRS - 1; i++) {
			struct net_v4_secondary *s = &v->sec[i];
			if (!s->used || s->label[0])
				continue;
			v->ip = s->ip;
			v->netmask = s->mask;
			memset(s, 0, sizeof(*s));
			break;
		}
		return 0;
	}
	for (usize i = 0; i < NET_V4_MAX_ADDRS - 1; i++) {
		struct net_v4_secondary *s = &v->sec[i];
		if (s->used && ip4_eq(s->ip, ip)) {
			memset(s, 0, sizeof(*s));
			return 0;
		}
	}
	return -EADDRNOTAVAIL;
}

int net_ipv4_is_local_ns(u32 ns, struct ipv4_addr ip)
{
	struct net_ns_ipv4 *v = net_v4(ns);

	if (ip4_is_zero(ip))
		return 0;
	if (ip4_eq(v->ip, ip))
		return 1;
	for (usize i = 0; i < NET_V4_MAX_ADDRS - 1; i++)
		if (v->sec[i].used && ip4_eq(v->sec[i].ip, ip))
			return 1;
	return 0;
}

/* The address on the peer's prefix, as Linux's inet_select_addr() picks it;
 * the primary when no address shares one (a destination behind a gateway, or
 * a broadcast); any address at all before there is a primary. */
struct ipv4_addr net_ipv4_source_for(u32 ns, struct ipv4_addr peer)
{
	struct net_ns_ipv4 *v = net_v4(ns);

	if (!ip4_is_zero(v->ip) && ip4_same_prefix(v->ip, peer, v->netmask))
		return v->ip;
	for (usize i = 0; i < NET_V4_MAX_ADDRS - 1; i++)
		if (v->sec[i].used && ip4_same_prefix(v->sec[i].ip, peer, v->sec[i].mask))
			return v->sec[i].ip;
	if (!ip4_is_zero(v->ip))
		return v->ip;
	for (usize i = 0; i < NET_V4_MAX_ADDRS - 1; i++)
		if (v->sec[i].used)
			return v->sec[i].ip;
	return v->ip;
}

usize net_ipv4_addr_list(u32 ns, struct net_v4_addr_info *out, usize max)
{
	struct net_ns_ipv4 *v = net_v4(ns);
	usize n = 0;

	if (!out)
		return 0;
	if (!ip4_is_zero(v->ip) && n < max) {
		memset(&out[n], 0, sizeof(out[n]));
		out[n].ip = v->ip;
		out[n].mask = v->netmask;
		n++;
	}
	for (usize i = 0; i < NET_V4_MAX_ADDRS - 1 && n < max; i++) {
		struct net_v4_secondary *s = &v->sec[i];
		if (!s->used)
			continue;
		memset(&out[n], 0, sizeof(out[n]));
		out[n].ip = s->ip;
		out[n].mask = s->mask;
		memcpy(out[n].label, s->label, sizeof(out[n].label));
		out[n].secondary = (u8)(!ip4_is_zero(v->ip) &&
		                        ip4_same_prefix(v->ip, s->ip, v->netmask));
		n++;
	}
	return n;
}

int net_ipv4_alias_get(u32 ns, const char *label, struct net_v4_addr_info *out)
{
	struct net_ns_ipv4 *v = net_v4(ns);

	if (!label || !label[0])
		return -EINVAL;
	for (usize i = 0; i < NET_V4_MAX_ADDRS - 1; i++) {
		struct net_v4_secondary *s = &v->sec[i];
		if (!s->used || strcmp(s->label, label) != 0)
			continue;
		if (out) {
			memset(out, 0, sizeof(*out));
			out->ip = s->ip;
			out->mask = s->mask;
			memcpy(out->label, s->label, sizeof(out->label));
		}
		return 0;
	}
	return -EADDRNOTAVAIL;
}

void net_ipv4_routes_refresh(u32 ns)
{
	struct net_ns_ipv4 *v = net_v4(ns);
	int any = !ip4_is_zero(v->ip);

	for (usize i = 0; i < NET_V4_MAX_ADDRS - 1; i++)
		if (v->sec[i].used)
			any = 1;
	if (!any) {
		route_flush_dynamic();
		return;
	}
	route_configure_interface(v->ip, v->netmask, v->gateway);
}

/* ── IPv6 addresses ────────────────────────────────────────────────────── */

static int in6_zero(const struct in6_addr_k *a)
{
	for (int i = 0; i < 16; i++)
		if (a->bytes[i])
			return 0;
	return 1;
}

static int in6_prefix_match(const struct in6_addr_k *a,
                            const struct in6_addr_k *b, u8 plen)
{
	u8 full = (u8)(plen / 8), rem = (u8)(plen % 8);

	if (plen > 128)
		return 0;
	if (memcmp(a->bytes, b->bytes, full) != 0)
		return 0;
	if (rem) {
		u8 m = (u8)(0xFF << (8 - rem));
		if ((a->bytes[full] & m) != (b->bytes[full] & m))
			return 0;
	}
	return 1;
}

static struct in6_addr_k ll_from_mac(struct mac_addr mac)
{
	struct in6_addr_k a;

	memset(&a, 0, sizeof(a));
	a.bytes[0] = 0xfe;
	a.bytes[1] = 0x80;
	a.bytes[8] = mac.bytes[0] ^ 0x02; /* flip U/L bit */
	a.bytes[9] = mac.bytes[1];
	a.bytes[10] = mac.bytes[2];
	a.bytes[11] = 0xff;
	a.bytes[12] = 0xfe;
	a.bytes[13] = mac.bytes[3];
	a.bytes[14] = mac.bytes[4];
	a.bytes[15] = mac.bytes[5];
	return a;
}

static struct in6_addr_k net_ip6_ll_ns(u32 ns)
{
	if (ns == 0)
		return local_ip6_ll;
	struct netdev *nd = netdev_active_ns(ns);
	if (!nd) {
		struct in6_addr_k zero;
		memset(&zero, 0, sizeof(zero));
		return zero;
	}
	return ll_from_mac(nd->mac);
}

struct in6_addr_k net_get_ip6_ll(void) { return net_ip6_ll_ns(net_ns_ctx()); }
struct in6_addr_k net_get_ip6(void) { return net_v6(net_ns_ctx())->global; }
struct in6_addr_k net_get_gateway6(void) { return net_v6(net_ns_ctx())->gateway; }
struct in6_addr_k net_get_prefix6(void) { return net_v6(net_ns_ctx())->prefix; }
int net_get_prefix6_valid(void) { return net_v6(net_ns_ctx())->prefix_valid; }
void net_set_ip6(struct in6_addr_k a) { net_v6(net_ns_ctx())->global = a; }
void net_set_gateway6(struct in6_addr_k a) { net_v6(net_ns_ctx())->gateway = a; }
void net_set_prefix6(struct in6_addr_k p)
{
	struct net_ns_ipv6 *v = net_v6(net_ns_ctx());
	v->prefix = p;
	v->prefix_valid = 1;
}

void net_ns_clear_ipv6(u32 ns)
{
	memset(net_v6(ns), 0, sizeof(struct net_ns_ipv6));
}

int net_ipv6_addr_add(u32 ns, struct in6_addr_k a, u8 plen)
{
	struct net_ns_ipv6 *v = net_v6(ns);
	int slot = -1;

	if (in6_zero(&a) || a.bytes[0] == 0xff || plen > 128)
		return -EINVAL;
	for (int i = 0; i < NET_V6_MAX_ADDRS; i++) {
		if (v->assigned[i].used &&
		    memcmp(v->assigned[i].addr.bytes, a.bytes, 16) == 0) {
			v->assigned[i].plen = plen;
			return 0;
		}
		if (!v->assigned[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return -ENOSPC;
	v->assigned[slot].used = 1;
	v->assigned[slot].addr = a;
	v->assigned[slot].plen = plen;
	return 0;
}

int net_ipv6_addr_del(u32 ns, struct in6_addr_k a)
{
	struct net_ns_ipv6 *v = net_v6(ns);

	for (int i = 0; i < NET_V6_MAX_ADDRS; i++) {
		if (v->assigned[i].used &&
		    memcmp(v->assigned[i].addr.bytes, a.bytes, 16) == 0) {
			memset(&v->assigned[i], 0, sizeof(v->assigned[i]));
			return 0;
		}
	}
	if (!in6_zero(&v->global) && memcmp(v->global.bytes, a.bytes, 16) == 0) {
		memset(&v->global, 0, sizeof(v->global));
		return 0;
	}
	return -EADDRNOTAVAIL;
}

usize net_ipv6_addr_list(u32 ns, struct net_v6_addr_info *out, usize max)
{
	struct net_ns_ipv6 *v = net_v6(ns);
	usize n = 0;
	struct in6_addr_k ll = net_ip6_ll_ns(ns);

	if (!out)
		return 0;
	if (!in6_zero(&ll) && n < max) {
		out[n].addr = ll;
		out[n].plen = 64;
		out[n].scope_link = 1;
		n++;
	}
	if (!in6_zero(&v->global) && n < max) {
		out[n].addr = v->global;
		out[n].plen = 64;
		out[n].scope_link = 0;
		n++;
	}
	for (int i = 0; i < NET_V6_MAX_ADDRS && n < max; i++) {
		if (!v->assigned[i].used)
			continue;
		out[n].addr = v->assigned[i].addr;
		out[n].plen = v->assigned[i].plen;
		out[n].scope_link = (u8)(v->assigned[i].addr.bytes[0] == 0xfe &&
		                         (v->assigned[i].addr.bytes[1] & 0xc0) == 0x80);
		n++;
	}
	return n;
}

int net_ip6_is_local(struct in6_addr_k a)
{
	u32 ns = net_ns_ctx();
	struct net_ns_ipv6 *v = net_v6(ns);
	struct in6_addr_k ll = net_ip6_ll_ns(ns);

	if (in6_zero(&a))
		return 0;
	if (memcmp(ll.bytes, a.bytes, 16) == 0)
		return 1;
	if (!in6_zero(&v->global) && memcmp(v->global.bytes, a.bytes, 16) == 0)
		return 1;
	for (int i = 0; i < NET_V6_MAX_ADDRS; i++)
		if (v->assigned[i].used &&
		    memcmp(v->assigned[i].addr.bytes, a.bytes, 16) == 0)
			return 1;
	return 0;
}

/* Loopback for ::1, the link-local for link-local and multicast peers, else
 * the assigned address on the destination's prefix, the SLAAC address, any
 * assigned address, and the link-local as the last resort. */
struct in6_addr_k net_ip6_source_for(struct in6_addr_k dst)
{
	u32 ns = net_ns_ctx();
	struct net_ns_ipv6 *v = net_v6(ns);
	int is_lo = 1;

	for (int i = 0; i < 15; i++)
		if (dst.bytes[i])
			is_lo = 0;
	if (is_lo && dst.bytes[15] == 1)
		return dst;
	if (dst.bytes[0] == 0xff || (dst.bytes[0] == 0xfe && (dst.bytes[1] & 0xc0) == 0x80))
		return net_ip6_ll_ns(ns);
	for (int i = 0; i < NET_V6_MAX_ADDRS; i++)
		if (v->assigned[i].used && v->assigned[i].plen &&
		    in6_prefix_match(&v->assigned[i].addr, &dst, v->assigned[i].plen))
			return v->assigned[i].addr;
	if (!in6_zero(&v->global))
		return v->global;
	for (int i = 0; i < NET_V6_MAX_ADDRS; i++)
		if (v->assigned[i].used)
			return v->assigned[i].addr;
	return net_ip6_ll_ns(ns);
}

static void net_compute_link_local(void);

static void net_reset_interface_state(struct netdev *nd)
{
	/* nd == NULL: no interface is active any more (every one of them was taken
	 * administratively down). Keep the last station address so /sys and ifconfig
	 * still report the hardware, but drop every L3 fact. */
	if (nd)
		local_mac = nd->mac;
	/* This is the initial namespace's interface changing under it. A namespace
	 * that was handed an interface keeps its own configuration; nothing here
	 * happened to it. */
	/* The FIB describes the old interface's topology; a switch invalidates
	 * every autoconfigured route. */
	route_flush_dynamic();
	route6_flush_dynamic();
	/* Secondary addresses described the old interface too. */
	net_ns_clear_ipv4(0);
	net_ns_clear_ipv6(0);
	net_compute_link_local();
	arp_init();
	net_proto_reset();
}

static void net_switch_active(struct netdev *nd)
{
	if (!nd || nd == g_netdev)
		return;
	dhcp_stop();
	g_netdev = nd;
	net_reset_interface_state(nd);
	net_irq_pending = 0;
	console_write("net: switched active driver to ");
	console_write(nd->name);
	console_write("\n");
}

/* `ip link set <if> up/down` / SIOCSIFFLAGS. Down is a real state change, not a
 * cosmetic flag: the interface stops transmitting and its received frames are
 * dropped, and if it was carrying the L3 configuration that role moves to
 * another interface (or the stack is left with none, which is what the operator
 * asked for). */
int netdev_set_admin_up(struct netdev *nd, int up)
{
	if (!nd)
		return -ENODEV;
	if (!nd->admin_down == !!up)
		return 0; /* already in the requested state */
	nd->admin_down = up ? 0 : 1;

	/* One call, not three.
	 *
	 * Interrupts are masked inside console_write but not BETWEEN calls, so a
	 * record assembled from several of them can be cut in half by a handler
	 * that prints -- and the tail then lands in the middle of somebody else's
	 * line while the next line starts with no timestamp, because the console
	 * believes it is still finishing this one. Six of these eight lines came
	 * out unstamped for exactly that reason. A log record is a record: build
	 * it, then write it. */
	{
		char line[96];

		snprintf(line, sizeof(line), "%s administratively %s",
		         nd->name, up ? "up" : "down");
		k_info("net", "%s", line);
	}

	if (!up && nd == g_netdev) {
		dhcp_stop();
		struct netdev *next = netdev_best();
		g_netdev = next;
		last_link_state = -2;
		net_irq_pending = 0;
		if (next) {
			net_reset_interface_state(next);
			if (networking_enabled)
				dhcp_init();
		} else {
			/* No interface left to hold an address. Drop the L3 state
			 * rather than keep answering for an address nothing can
			 * reach. */
			net_reset_interface_state(0);
		}
		return 0;
	}

	if (up && !g_netdev) {
		g_netdev = nd;
		last_link_state = -2;
		net_reset_interface_state(nd);
		if (networking_enabled)
			dhcp_init();
	}
	return 0;
}

int net_dhcp_try_failover(void)
{
	if (g_netdev_count < 2)
		return 0;

	usize active_index = 0;
	for (usize i = 0; i < g_netdev_count; i++) {
		if (g_netdevs[i] == g_netdev) {
			active_index = i;
			break;
		}
	}

	/* Prefer a definite carrier, then allow devices whose driver cannot report
	 * carrier. Walk from the current interface so registration order is only a
	 * starting point, not a permanent preference. */
	for (int pass = 0; pass < 2; pass++) {
		for (usize step = 1; step < g_netdev_count; step++) {
			struct netdev *candidate =
				g_netdevs[(active_index + step) % g_netdev_count];
			if (!candidate || netdev_is_virtual(candidate))
				continue;
			int link = netdev_link_state(candidate);
			if ((pass == 0 && link != 1) || (pass == 1 && link >= 0))
				continue;
			net_switch_active(candidate);
			last_link_state = link;
			if (networking_enabled)
				dhcp_init();
			return 1;
		}
	}
	return 0;
}

/* Build the EUI-64 modified interface identifier from the 48-bit MAC and
 * compose the fe80::/64 link-local address. */
static void net_compute_link_local(void)
{
	local_ip6_ll = ll_from_mac(local_mac);
}

int net_is_ready(void) { return netdev_active() != 0; }


int net_get_irq(void)
{
	struct netdev *nd = netdev_active();
	return nd ? nd->irq : -1;
}

/* Service a device interrupt on `irq`. Every registered interface on that line
 * is acknowledged, not just the active one: PCI INTx lines are shared and
 * level-triggered, so a standby NIC whose cause register is never read would
 * hold the line asserted and livelock the CPU. Returns 1 if any interface
 * claimed the interrupt. */
int net_handle_irq(int irq)
{
	int claimed = 0;
	for (usize i = 0; i < g_netdev_count; i++) {
		struct netdev *nd = g_netdevs[i];
		if (!nd || !nd->irq_ack || nd->irq != irq)
			continue;
		if (nd->irq_ack(nd))
			claimed = 1;
	}
	if (claimed) {
		net_irq_pending = 1;
		/* M70: wake net_task immediately so RX is drained on packet arrival
		 * instead of waiting up to a full ~100Hz poll tick. */
		if (net_task_id >= 0)
			scheduler_wake_task((usize)net_task_id);
	}
	return claimed;
}

void net_interrupt_handler(void)
{
	struct netdev *nd = netdev_active();
	if (!nd || !nd->irq_ack) return;
	if (nd->irq_ack(nd)) {
		net_irq_pending = 1;
		/* M70: wake net_task immediately so RX is drained on packet arrival
		 * instead of waiting up to a full ~100Hz poll tick. The daemon sleeps
		 * between polls (see net_task) and scheduler_wake_task promotes it out
		 * of SLEEPING; the 1-tick timeout still fires the TCP/DHCP/NDP timers on
		 * cadence when no packet arrives. */
		if (net_task_id >= 0)
			scheduler_wake_task((usize)net_task_id);
	}
}

/* ── PCI adapter inventory (for `ifconfig`/`net` listing) ───────────────── */

static void net_record_pci_class(u8 subclass)
{
	for (u8 idx = 0; net_adapter_count < NET_MAX_ADAPTERS; idx++) {
		struct pci_device_info pci;
		if (!pci_find_class(0x02, subclass, idx, &pci)) {
			break;
		}

		struct net_adapter *adapter = &net_adapters[net_adapter_count++];
		adapter->pci = pci;
		for (u8 bar = 0; bar < 6; bar++) {
			adapter->bars[bar] = pci_config_read32(pci.bus, pci.slot, pci.func, (u8)(0x10 + bar * 4));
		}
	}
}

static void net_scan_pci_adapters(void)
{
	net_adapter_count = 0;
	net_record_pci_class(0x00);
	net_record_pci_class(0x80);
}

/* ── net_task daemon ────────────────────────────────────────────────────── */

static void net_task(void *arg)
{
	u64 idle_ticks = 1;

	(void)arg;
	while (1) {
		struct netdev *best = netdev_best();
		if (best != g_netdev && netdev_link_state(g_netdev) != 1)
			net_switch_active(best);

		/* No interface at all: there is no carrier to report a change in, and
		 * saying "link down" about a machine that has no link is noise. The
		 * loopback drain below is why this daemon still runs. */
		int link = g_netdev ? netdev_link_state(g_netdev) : last_link_state;
		if (link != last_link_state) {
			if (link == 0) {
				k_info("net", "link down");
				dhcp_stop();
			} else {
				console_write(link > 0 ? "net: link up\n"
				                       : "net: link state unknown\n");
				if (networking_enabled)
					dhcp_init();
			}
			last_link_state = link;
		}
		if (net_irq_pending) {
			net_irq_pending = 0;
		}
		net_poll();
		dhcp_tick(scheduler_get_uptime_ticks());
		/* M96: NTP and NDP tick through the protocol registry — both live in
		 * loadable modules and are simply absent when they are not loaded.
		 * DHCPv6 is still built into the kernel, so it keeps its direct call. */
		net_proto_tick(scheduler_get_uptime_ticks());
		dhcpv6_tick(scheduler_get_uptime_ticks());
		/* Sleep between polls rather than busy-yielding. As a perpetually
		 * runnable kernel daemon, busy-yielding would keep net_task READY and —
		 * under the Big Kernel Lock — let it monopolise the lock across its
		 * cooperative yields (it never enters ring 3 to release it), starving
		 * userspace on the other cores.
		 *
		 * The interval backs off when there is no traffic. A fixed one-tick
		 * sleep made this daemon the machine's heartbeat: measured on the
		 * aarch64 sys lane, 4,431 of 4,438 idle ticks were ended by this wake,
		 * over 4,422 separate ~10 ms waits, and it kept the one-shot timer from
		 * ever programming a longer sleep because there was always a deadline
		 * one tick out.
		 *
		 * Backing off is safe because nothing here is the packet path: both the
		 * NIC interrupt (M70) and net_loopback_enqueue wake this task
		 * explicitly. What the beat is actually for is the protocol
		 * housekeeping below -- DHCP, NDP, NTP, TCP retransmit -- and those
		 * work in seconds and check their own deadlines. This is Linux's
		 * TIMER_DEFERRABLE in spirit: a timer that must not be the reason an
		 * idle machine wakes up. */
		u64 until = scheduler_get_ticks() + idle_ticks;

		scheduler_sleep_ticks(idle_ticks);

		/* Reset the backoff only when something actually woke us EARLY, or
		 * there is work waiting -- not merely because a flag happened to be
		 * set when the loop came round.
		 *
		 * Waking before the deadline is the exact signal for "an event, not my
		 * timer", and it needs no new plumbing to ask, so this is the right
		 * test to be making.
		 *
		 * It is NOT, however, the fix it looks like: measured, it changed
		 * nothing. net_task still averages 2.3 ticks a sleep and 1,679 of its
		 * 1,688 wakes are its own timer finding no work, exactly as before.
		 * Something resets this backoff that is neither an early wake nor a
		 * pending packet, and what that is has not been established yet. Do not
		 * read this comment as saying the beat was fixed. */
		int early = scheduler_get_ticks() < until;
		int irq = net_irq_pending != 0;
		int lb = net_loopback_pending();

		net_beat_count(early, irq, lb, idle_ticks);
		if (early || irq || lb)
			idle_ticks = 1;
		else if (idle_ticks < net_idle_tick_cap())
			idle_ticks *= 2;
	}
}

void net_init(void)
{
	g_netdev = 0;
	g_receiving_netdev = 0;
	g_netdev_count = 0;
	memset(g_netdevs, 0, sizeof(g_netdevs));
	memset(&local_mac, 0, sizeof(local_mac));
	memset(net_ns_v4, 0, sizeof(net_ns_v4));
	memset(net_ns_v6, 0, sizeof(net_ns_v6));
	/* Default policy (everything looks in the main table) plus the standing
	 * on-link IPv6 routes: fe80::/10, ::1/128 and ff02::/16 exist by
	 * definition, so NDP works before any router advertisement. */
	route_init();
	route6_flush_all();
	route6_init();
	net_scan_pci_adapters();

	/* Probe every supported NIC, then prefer one whose PHY reports carrier. */
#if defined(__aarch64__)
	/* QEMU virt has no PCI host bridge wired up here: the NIC arrives over the
	 * virtio-mmio transport, same as the block device. */
	{
		extern int virtio_net_mmio_init(void);
		virtio_net_mmio_init();
	}
#endif
	virtio_net_probe();
	e1000_probe();
	r8169_probe();   /* Realtek RTL8169/8168/8111/810x family (e.g. ZG5 RTL8102E) */

	g_netdev = netdev_best();
	struct netdev *nd = netdev_active();
	console_write("net: pci adapters 0x");
	console_write_hex64(net_adapter_count);
	console_write(", driver ");
	console_write(nd ? nd->name : "none");
	console_write("\n");

	if (nd) {
		net_reset_interface_state(nd);
	} else {
		arp_init();
		net_proto_reset();
	}

	/* The pump runs even with no NIC: loopback delivery is deferred onto a
	 * queue that only net_poll() drains, so without this daemon every
	 * 127.0.0.1 connect/accept blocks forever. x86_64 never noticed because
	 * the smoke instances always have a virtio-net/e1000 attached; aarch64
	 * (QEMU virt, no PCI NIC) hung in net_smoke's TCP loopback test. */
	if (!nd) {
		net_task_id = kthread_create("net_task", net_task, 0);
		return;
	}
	/* Networking is on by default: bring the link up via DHCP whenever a NIC is
	 * present. Opt out with b1nix.net=off (or b1nix.nonet) for an isolated boot;
	 * b1nix.net=dhcp is still accepted as an explicit no-op for back-compat. */
	networking_enabled = nd && !bootinfo_has_flag("b1nix.net=off") &&
	                     !bootinfo_has_flag("b1nix.nonet");
	if (nd) {
		last_link_state = netdev_link_state(nd);
		if (networking_enabled && last_link_state != 0) {
			dhcp_init();
		} else if (networking_enabled && last_link_state == 0) {
			k_info("net", "waiting for link");
		}
	}

	/* Unconditionally, even with no NIC at all: net_task is the only thing that
	 * drains the loopback queue in a clean context. ipv4_send_tx does not
	 * deliver a datagram addressed to 127.0.0.0/8 synchronously -- that would
	 * re-enter the TCP state machine mid-send -- it queues it and wakes this
	 * daemon. Without the daemon the queue is drained only by the net_poll()
	 * calls inside TCP's own blocking waits, so a NON-blocking loopback
	 * connection (which is what every event-loop program makes) had nothing to
	 * move it: the SYN sat in the queue until TCP gave up minutes later.
	 * Loopback is a property of the stack, not of the hardware. */
	net_task_id = kthread_create("net_task", net_task, 0);
}

void net_send_ethernet_dev(struct netdev *nd, struct mac_addr dst,
                           u16 ethertype, const void *payload, usize size)
{
	net_send_ethernet_tx(nd, dst, ethertype, payload, size, 0);
}

void net_send_ethernet_tx(struct netdev *nd, struct mac_addr dst,
                          u16 ethertype, const void *payload, usize size,
                          u32 tx_flags)
{
	if (!nd)
		nd = netdev_active();
	if (!nd || !nd->transmit) {
		return;
	}
	/* An administratively down interface does not transmit. */
	if (nd->admin_down)
		return;

	u8 hdr[14];
	memcpy(hdr, dst.bytes, 6);
	/* The source MAC must be the transmitting device's own address, which is
	 * only the cached local_mac when that device is the active one. */
	memcpy(hdr + 6, (nd == g_netdev) ? local_mac.bytes : nd->mac.bytes, 6);
	hdr[12] = (ethertype >> 8) & 0xFF;
	hdr[13] = ethertype & 0xFF;

	if (nd->transmit(nd, hdr, payload, size, tx_flags) == 0)
		packet_socket_tx(nd, hdr, payload, size);
}

void net_send_ethernet(struct mac_addr dst, u16 ethertype, const void *payload, usize size)
{
	net_send_ethernet_tx(netdev_active(), dst, ethertype, payload, size, 0);
}

int netdev_transmit_frame(struct netdev *nd, const u8 hdr[14],
                          const void *payload, usize payload_len, u32 tx_flags)
{
	if (!nd || !nd->transmit)
		return -ENODEV;
	if (nd->admin_down)
		return -ENETDOWN;
	int rc = nd->transmit(nd, hdr, payload, payload_len, tx_flags);
	if (rc == 0)
		packet_socket_tx(nd, hdr, payload, payload_len);
	return rc;
}

/* Bound on how deeply one received frame may be re-delivered: a VLAN on a
 * bridge port on a bond is three, and anything past that is a stacking loop
 * rather than a configuration. */
#define NET_RX_MAX_DEPTH 4
static volatile int net_rx_depth;

void net_deliver_frame(struct netdev *dev, const void *frame, usize len,
                       u32 rx_flags)
{
	if (!dev || !frame || len < 14)
		return;
	if (__atomic_fetch_add(&net_rx_depth, 1, __ATOMIC_ACQUIRE) >=
	    NET_RX_MAX_DEPTH) {
		__atomic_fetch_sub(&net_rx_depth, 1, __ATOMIC_RELEASE);
		return;
	}
	struct netdev *prev = g_receiving_netdev;
	g_receiving_netdev = dev;
	u32 prev_ns = namespace_net_push_context(dev->netns);
	ethernet_receive_flags(frame, len, rx_flags);
	namespace_net_pop_context(prev_ns);
	g_receiving_netdev = prev;
	__atomic_fetch_sub(&net_rx_depth, 1, __ATOMIC_RELEASE);
}

/* ── Loopback deferral queue (see net.h) ── */
/* Depth of the ring, in packets. Linux's equivalent — the per-CPU backlog a
 * loopback packet is queued on — is net.core.netdev_max_backlog, 1000 by
 * default; this is the same number. Each slot is a pointer, a length and a
 * flag, so the ring costs 24 bytes apiece and the packet payloads are the
 * kmalloc'd copies below. A full ring drops, and the drop is invisible to the
 * sender: only a retransmit recovers it, which is why the depth should be the
 * one Linux found sufficient rather than a quarter of it. */
#define NET_LOOPBACK_Q 1000
/* `ns` is the sender's network namespace: the queue is drained by whichever
 * task gets there, and a packet resolved in the drainer's namespace missed its
 * socket -- loopback TCP inside a namespace saw every data segment reset. */
struct net_loopback_pkt { u8 *data; usize len; int is_v6; u32 ns; };
static struct net_loopback_pkt net_loopback_q[NET_LOOPBACK_Q];
static volatile u32 net_lb_head; /* consumer */
static volatile u32 net_lb_tail; /* producer */

static int net_loopback_pending(void)
{
	return __atomic_load_n(&net_lb_head, __ATOMIC_RELAXED) !=
	       __atomic_load_n(&net_lb_tail, __ATOMIC_RELAXED);
}
static volatile int net_lb_lock;
static volatile int net_lb_draining = 0;

void net_loopback_enqueue(const void *ip_pkt, usize len, int is_v6)
{
	int kick_net_task = 0;

	if (!ip_pkt || len == 0)
		return;
	u8 *copy = kmalloc(len);
	if (!copy)
		return; /* drop on OOM — TCP retransmit recovers */
	memcpy(copy, ip_pkt, len);
	while (__atomic_test_and_set(&net_lb_lock, __ATOMIC_ACQUIRE)) { }
	u32 next = (net_lb_tail + 1) % NET_LOOPBACK_Q;
	if (next == net_lb_head) { /* full — drop, retransmit recovers */
		__atomic_clear(&net_lb_lock, __ATOMIC_RELEASE);
		kfree(copy);
		return;
	}
	net_loopback_q[net_lb_tail].data = copy;
	net_loopback_q[net_lb_tail].len = len;
	net_loopback_q[net_lb_tail].is_v6 = is_v6;
	net_loopback_q[net_lb_tail].ns = namespace_net_context();
	net_lb_tail = next;
	if (!__atomic_load_n(&net_lb_draining, __ATOMIC_ACQUIRE))
		kick_net_task = 1;
	__atomic_clear(&net_lb_lock, __ATOMIC_RELEASE);

	if (kick_net_task && net_task_id >= 0) {
		scheduler_wake_task((usize)net_task_id);
		ipi_reschedule_all();
	}
}

void net_loopback_drain(void)
{
	if (__atomic_test_and_set(&net_lb_draining, __ATOMIC_ACQUIRE)) {
		return;
	}
	while (1) {
		while (__atomic_test_and_set(&net_lb_lock, __ATOMIC_ACQUIRE)) { }
		if (net_lb_head == net_lb_tail) {
			__atomic_clear(&net_lb_draining, __ATOMIC_RELEASE);
			__atomic_clear(&net_lb_lock, __ATOMIC_RELEASE);
			break;
		}
		u8 *data = net_loopback_q[net_lb_head].data;
		usize len = net_loopback_q[net_lb_head].len;
		int is_v6 = net_loopback_q[net_lb_head].is_v6;
		u32 ns = net_loopback_q[net_lb_head].ns;
		net_lb_head = (net_lb_head + 1) % NET_LOOPBACK_Q;
		__atomic_clear(&net_lb_lock, __ATOMIC_RELEASE);

		u32 saved_ns = namespace_net_push_context(ns);
		if (is_v6)
			proto_deliver_ether(0x86DD, data, len);
		else
			/* Verified like anything else: the IP layer checksums a loopback
			 * datagram against the same source address it stamped into the
			 * header, so every packet the stack sends to itself also proves the
			 * software checksum path on the way back in. */
			ipv4_receive(data, len);
		namespace_net_pop_context(saved_ns);
		kfree(data);
	}
}

void net_loopback_hold(void)
{
	/* The drain flag is the hold: whoever owns it is the only drainer, and
	 * the net task backs off while it is set. Wait out a drain in progress. */
	while (__atomic_test_and_set(&net_lb_draining, __ATOMIC_ACQUIRE))
		scheduler_yield();
}

void net_loopback_release(void)
{
	__atomic_clear(&net_lb_draining, __ATOMIC_RELEASE);
	net_loopback_drain();
}

void net_poll(void)
{
	/* Drain loopback first, and unconditionally — loopback must work even
	 * before/without a NIC (the guard below would otherwise skip it). */
	net_loopback_drain();

	if (!g_netdev)
		return;

	tcp_timer_tick();

	for (usize i = 0; i < g_netdev_count; i++) {
		struct netdev *polled = g_netdevs[i];
		if (!polled || !polled->poll)
			continue;
		g_receiving_netdev = polled;
		u32 prev_ns = namespace_net_push_context(polled->netns);
		polled->poll(polled);
		namespace_net_pop_context(prev_ns);
	}
	g_receiving_netdev = 0;
}
