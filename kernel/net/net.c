#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/net.h>
#include <b1nix/netproto.h>
#include <b1nix/netdev.h>
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
#define NET_MAX_NETDEVS 8

static struct netdev *g_netdev;
static struct netdev *g_receiving_netdev;
static struct netdev *g_netdevs[NET_MAX_NETDEVS];
static usize g_netdev_count;

void netdev_register(struct netdev *nd)
{
	if (!nd)
		return;
	for (usize i = 0; i < g_netdev_count; i++) {
		if (g_netdevs[i] == nd)
			return;
	}
	if (g_netdev_count < NET_MAX_NETDEVS)
		g_netdevs[g_netdev_count++] = nd;
	if (!g_netdev)
		g_netdev = nd;
}

struct netdev *netdev_active(void) { return g_netdev; }
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
	return g_netdevs[idx - 1];
}

void netdev_ifname(int idx, char *out, usize cap)
{
	if (!out || cap < 6)
		return;
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
	if (!name || name[0] != 'e' || name[1] != 't' || name[2] != 'h')
		return 0;
	if (name[3] < '0' || name[3] > '9' || name[4] != '\0')
		return 0;
	int idx = (name[3] - '0') + 1;
	return netdev_by_index(idx) ? idx : 0;
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
	 * carrier says — that is the whole point of taking it down. */
	for (usize i = 0; i < g_netdev_count; i++) {
		if (!g_netdevs[i]->admin_down &&
		    netdev_link_state(g_netdevs[i]) == 1)
			return g_netdevs[i];
	}
	for (usize i = 0; i < g_netdev_count; i++) {
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
static struct ipv4_addr local_ip = { { 0, 0, 0, 0 } };
static struct ipv4_addr gateway_ip = { { 0, 0, 0, 0 } };
/* M84: the interface netmask is real state now (DHCP option 1), not a /24
 * assumption baked into ipv4_send/procfs. It defines the on-link prefix the
 * FIB installs. */
static struct ipv4_addr netmask_ip = { { 0, 0, 0, 0 } };

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

struct mac_addr net_get_mac(void) { return local_mac; }
struct ipv4_addr net_get_ip(void) { return local_ip; }
struct ipv4_addr net_get_gateway(void) { return gateway_ip; }
struct ipv4_addr net_get_netmask(void) { return netmask_ip; }
void net_set_ip(struct ipv4_addr ip) { local_ip = ip; }
void net_set_gateway(struct ipv4_addr gw) { gateway_ip = gw; }
void net_set_netmask(struct ipv4_addr mask) { netmask_ip = mask; }

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
	local_ip = zero4;
	gateway_ip = zero4;
	netmask_ip = zero4;
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
	print_ipv4(local_ip);
	console_write("\n gateway:");
	console_putc(' ');
	print_ipv4(gateway_ip);
	console_write(" [raw:");
	console_write_hex32(((u32)gateway_ip.bytes[0] << 24) |
	                    ((u32)gateway_ip.bytes[1] << 16) |
	                    ((u32)gateway_ip.bytes[2] <<  8) |
	                     (u32)gateway_ip.bytes[3]);
	console_write("]");
	console_write("\n");
	dhcp_dump_info();

	if (net_adapter_count == 0) {
		console_write(" pci:    no network adapters found\n");
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

		int link = netdev_link_state(g_netdev);
		if (link != last_link_state) {
			if (link == 0) {
				console_write("net: link down\n");
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
	local_ip = (struct ipv4_addr){{0, 0, 0, 0}};
	gateway_ip = (struct ipv4_addr){{0, 0, 0, 0}};
	netmask_ip = (struct ipv4_addr){{0, 0, 0, 0}};
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

	if (!nd) {
		return;
	}
	/* Networking is on by default: bring the link up via DHCP whenever a NIC is
	 * present. Opt out with b1nix.net=off (or b1nix.nonet) for an isolated boot;
	 * b1nix.net=dhcp is still accepted as an explicit no-op for back-compat. */
	networking_enabled = !bootinfo_has_flag("b1nix.net=off") &&
	                     !bootinfo_has_flag("b1nix.nonet");
	last_link_state = netdev_link_state(nd);
	if (networking_enabled && last_link_state != 0) {
		dhcp_init();
	} else if (networking_enabled && last_link_state == 0) {
		console_write("net: waiting for link\n");
	}

	net_task_id = kthread_create("net_task", net_task, 0);
}

void net_send_ethernet_dev(struct netdev *nd, struct mac_addr dst,
                           u16 ethertype, const void *payload, usize size)
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
	memcpy(hdr + 6, (nd == netdev_active()) ? local_mac.bytes : nd->mac.bytes, 6);
	hdr[12] = (ethertype >> 8) & 0xFF;
	hdr[13] = ethertype & 0xFF;

	nd->transmit(nd, hdr, payload, size);
}

void net_send_ethernet(struct mac_addr dst, u16 ethertype, const void *payload, usize size)
{
	struct netdev *nd = netdev_active();
	if (!nd || !nd->transmit) {
		return;
	}
	if (nd->admin_down)
		return;

	u8 hdr[14];
	memcpy(hdr, dst.bytes, 6);
	memcpy(hdr + 6, local_mac.bytes, 6);
	hdr[12] = (ethertype >> 8) & 0xFF;
	hdr[13] = ethertype & 0xFF;

	nd->transmit(nd, hdr, payload, size);
}

/* ── Loopback deferral queue (see net.h) ── */
#define NET_LOOPBACK_Q 256
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
			ipv4_receive(data, len);
		kfree(data);
	}
}

void net_poll(void)
{
	/* Drain loopback first, and unconditionally — loopback must work even
	 * before/without a NIC (the guard below would otherwise skip it). */
	net_loopback_drain();

	struct netdev *nd = netdev_active();
	if (!nd) {
		return;
	}

	tcp_timer_tick();

	for (usize i = 0; i < g_netdev_count; i++) {
		struct netdev *polled = g_netdevs[i];
		if (!polled || !polled->poll)
			continue;
		g_receiving_netdev = polled;
		polled->poll(polled);
	}
	g_receiving_netdev = 0;
}
