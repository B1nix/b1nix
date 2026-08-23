#include <b1nix/kprintf.h>
#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/namespace.h>
#include <b1nix/net.h>
#include <b1nix/netproto.h>
#include <b1nix/netdev.h>
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
/* Sixteen, not eight: a veth pair costs two slots and a namespace usually gets
 * one pair on top of whatever the initial namespace already holds. */
#define NET_MAX_NETDEVS 16

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
	if (from != 0 && !netdev_active_ns(from))
		net_ns_clear_ipv4(from);
	return 0;
}

/* Tear a network namespace down: everything created in it goes with it, and a
 * physical NIC that was moved into it returns to the initial namespace rather
 * than becoming unreachable (this is what Linux does too). */
void net_ns_destroy(u32 ns)
{
	if (ns == 0)
		return;
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
	struct ipv4_addr ip = net_get_ip_ns(nd->netns);
	return (ip.bytes[0] | ip.bytes[1] | ip.bytes[2] | ip.bytes[3]) != 0;
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
struct net_ns_ipv4 {
	struct ipv4_addr ip;
	struct ipv4_addr gateway;
	struct ipv4_addr netmask;
};

static struct net_ns_ipv4 net_ns_v4[NS_MAX_NET];

static struct net_ns_ipv4 *net_v4(u32 ns)
{
	/* An out-of-range id can only come from a caller that invented one; the
	 * initial namespace is the safe answer and never a silent write into
	 * another namespace's state. */
	return &net_ns_v4[ns < NS_MAX_NET ? ns : 0];
}

/* IPv6 interface state: link-local is derived from the MAC at probe time; the
 * global address / prefix / gateway are filled in by SLAAC (see ndp.c). */
static struct in6_addr_k local_ip6_ll;       /* fe80::/64 link-local */
static struct in6_addr_k local_ip6;          /* global (SLAAC), 0 until set */
static struct in6_addr_k gateway_ip6;        /* router link-local */
static struct in6_addr_k prefix6;            /* on-link /64 prefix */
static int prefix6_valid;

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

struct in6_addr_k net_get_ip6_ll(void) { return local_ip6_ll; }
struct in6_addr_k net_get_ip6(void) { return local_ip6; }
struct in6_addr_k net_get_gateway6(void) { return gateway_ip6; }
struct in6_addr_k net_get_prefix6(void) { return prefix6; }
int net_get_prefix6_valid(void) { return prefix6_valid; }
void net_set_ip6(struct in6_addr_k a) { local_ip6 = a; }
void net_set_gateway6(struct in6_addr_k a) { gateway_ip6 = a; }
void net_set_prefix6(struct in6_addr_k p) { prefix6 = p; prefix6_valid = 1; }

static void net_compute_link_local(void);

static void net_reset_interface_state(struct netdev *nd)
{
	struct ipv4_addr zero4 = {{0, 0, 0, 0}};

	/* nd == NULL: no interface is active any more (every one of them was taken
	 * administratively down). Keep the last station address so /sys and ifconfig
	 * still report the hardware, but drop every L3 fact. */
	if (nd)
		local_mac = nd->mac;
	/* This is the initial namespace's interface changing under it. A namespace
	 * that was handed an interface keeps its own configuration; nothing here
	 * happened to it. */
	net_set_ip_ns(0, zero4);
	net_set_gateway_ns(0, zero4);
	net_set_netmask_ns(0, zero4);
	/* The FIB describes the old interface's topology; a switch invalidates
	 * every autoconfigured route. */
	route_flush_dynamic();
	route6_flush_dynamic();
	memset(&local_ip6, 0, sizeof(local_ip6));
	memset(&gateway_ip6, 0, sizeof(gateway_ip6));
	memset(&prefix6, 0, sizeof(prefix6));
	prefix6_valid = 0;
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

	console_write("net: ");
	console_write(nd->name);
	console_write(up ? " administratively up\n" : " administratively down\n");

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
	memset(&local_ip6_ll, 0, sizeof(local_ip6_ll));
	local_ip6_ll.bytes[0] = 0xfe;
	local_ip6_ll.bytes[1] = 0x80;
	local_ip6_ll.bytes[8] = local_mac.bytes[0] ^ 0x02; /* flip U/L bit */
	local_ip6_ll.bytes[9] = local_mac.bytes[1];
	local_ip6_ll.bytes[10] = local_mac.bytes[2];
	local_ip6_ll.bytes[11] = 0xff;
	local_ip6_ll.bytes[12] = 0xfe;
	local_ip6_ll.bytes[13] = local_mac.bytes[3];
	local_ip6_ll.bytes[14] = local_mac.bytes[4];
	local_ip6_ll.bytes[15] = local_mac.bytes[5];
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

static const char *net_vendor_name(u16 vendor)
{
	switch (vendor) {
	case 0x10ec: return "Realtek";
	case 0x14e4: return "Broadcom";
	case 0x168c: return "Qualcomm/Atheros";
	case 0x1969: return "Atheros";
	case 0x1af4: return "VirtIO";
	case 0x8086: return "Intel";
	default: return "unknown";
	}
}

static const char *net_kind_name(u8 subclass)
{
	switch (subclass) {
	case 0x00: return "Ethernet";
	case 0x80: return "network";
	default: return "network";
	}
}

static void print_hex8(u8 value)
{
	const char *digits = "0123456789abcdef";
	console_putc(digits[(value >> 4) & 0xf]);
	console_putc(digits[value & 0xf]);
}

static void print_ipv4(struct ipv4_addr addr)
{
	for (int i = 0; i < 4; i++) {
		console_write_dec(addr.bytes[i]);
		if (i < 3) console_putc('.');
	}
}

static void print_mac(struct mac_addr mac)
{
	for (int i = 0; i < 6; i++) {
		print_hex8(mac.bytes[i]);
		if (i < 5) console_putc(':');
	}
}

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

void net_dump_info(void)
{
	struct netdev *nd = netdev_active();
	console_write("Network\n");
	console_write(" driver: ");
	console_write(nd ? nd->name : "none");
	console_write("\n link:   ");
	int link = netdev_link_state(nd);
	console_write(link > 0 ? "up" : (link == 0 ? "down" : "unknown"));
	console_write("\n mac:    ");
	print_mac(local_mac);
	console_write("\n ip:     ");
	print_ipv4(net_get_ip());
	console_write("\n gateway:");
	console_putc(' ');
	struct ipv4_addr gw = net_get_gateway();
	print_ipv4(gw);
	console_write(" [raw:");
	console_write_hex32(((u32)gw.bytes[0] << 24) |
	                    ((u32)gw.bytes[1] << 16) |
	                    ((u32)gw.bytes[2] <<  8) |
	                     (u32)gw.bytes[3]);
	console_write("]");
	console_write("\n");
	dhcp_dump_info();

	if (net_adapter_count == 0) {
		k_info(NULL, " pci:    no network adapters found");
		return;
	}

	for (usize i = 0; i < net_adapter_count; i++) {
		const struct net_adapter *adapter = &net_adapters[i];
		const struct pci_device_info *pci = &adapter->pci;
		console_write(" pci:    ");
		console_write_dec(pci->bus);
		console_putc(':');
		console_write_dec(pci->slot);
		console_putc('.');
		console_write_dec(pci->func);
		console_putc(' ');
		console_write(net_vendor_name(pci->vendor_id));
		console_putc(' ');
		console_write(net_kind_name(pci->subclass));
		console_write(" vendor 0x");
		console_write_hex32(pci->vendor_id);
		console_write(" device 0x");
		console_write_hex32(pci->device_id);
		console_write(" prog_if 0x");
		console_write_hex32(pci->prog_if);
		console_write("\n");
	}
}

/* ── net_task daemon ────────────────────────────────────────────────────── */

static void net_task(void *arg)
{
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
		/* Sleep a tick between polls rather than busy-yielding. As a perpetually
		 * runnable kernel daemon, busy-yielding would keep net_task READY and —
		 * under the Big Kernel Lock — let it monopolise the lock across its
		 * cooperative yields (it never enters ring 3 to release it), starving
		 * userspace on the other cores. Sleeping makes it BLOCKED between ~100Hz
		 * polls, which the DHCP/ARP/ICMP/UDP smoke paths tolerate. */
		scheduler_sleep_ticks(1);
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
	/* Default policy (everything looks in the main table) plus the standing
	 * on-link IPv6 routes: fe80::/10, ::1/128 and ff02::/16 exist by
	 * definition, so NDP works before any router advertisement. */
	route_init();
	route6_flush_all();
	route6_init();
	net_scan_pci_adapters();

	/* Probe every supported NIC, then prefer one whose PHY reports carrier. */
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
struct net_loopback_pkt { u8 *data; usize len; int is_v6; };
static struct net_loopback_pkt net_loopback_q[NET_LOOPBACK_Q];
static volatile u32 net_lb_head; /* consumer */
static volatile u32 net_lb_tail; /* producer */
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
		net_lb_head = (net_lb_head + 1) % NET_LOOPBACK_Q;
		__atomic_clear(&net_lb_lock, __ATOMIC_RELEASE);

		if (is_v6)
			proto_deliver_ether(0x86DD, data, len);
		else
			/* Verified like anything else: the IP layer checksums a loopback
			 * datagram against the same source address it stamped into the
			 * header, so every packet the stack sends to itself also proves the
			 * software checksum path on the way back in. */
			ipv4_receive(data, len);
		kfree(data);
	}
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
