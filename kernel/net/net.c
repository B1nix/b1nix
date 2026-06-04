#include <b1nix/console.h>
#include <b1nix/net.h>
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
static struct netdev *g_netdev;

void netdev_register(struct netdev *nd)
{
	/* First driver to register wins (probe order in net_init sets preference). */
	if (!g_netdev) g_netdev = nd;
}

struct netdev *netdev_active(void) { return g_netdev; }

/* ── Interface address state ────────────────────────────────────────────── */
static volatile int net_irq_pending = 0;
static volatile int net_task_id = -1;

static struct mac_addr local_mac;
static struct ipv4_addr local_ip = { { 0, 0, 0, 0 } };
static struct ipv4_addr gateway_ip = { { 0, 0, 0, 0 } };

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
void net_set_ip(struct ipv4_addr ip) { local_ip = ip; }
void net_set_gateway(struct ipv4_addr gw) { gateway_ip = gw; }

struct in6_addr_k net_get_ip6_ll(void) { return local_ip6_ll; }
struct in6_addr_k net_get_ip6(void) { return local_ip6; }
struct in6_addr_k net_get_gateway6(void) { return gateway_ip6; }
struct in6_addr_k net_get_prefix6(void) { return prefix6; }
int net_get_prefix6_valid(void) { return prefix6_valid; }
void net_set_ip6(struct in6_addr_k a) { local_ip6 = a; }
void net_set_gateway6(struct in6_addr_k a) { gateway_ip6 = a; }
void net_set_prefix6(struct in6_addr_k p) { prefix6 = p; prefix6_valid = 1; }

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
	if (nd->irq_ack(nd))
		net_irq_pending = 1;
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
	console_write(nd ? "up" : "down");
	console_write("\n mac:    ");
	print_mac(local_mac);
	console_write("\n ip:     ");
	print_ipv4(local_ip);
	console_write("\n gateway:");
	console_putc(' ');
	print_ipv4(gateway_ip);
	console_write("\n");

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
	u64 last_dhcp_retry = 0;
	while (1) {
		if (net_irq_pending) {
			net_irq_pending = 0;
		}
		net_poll();
		dhcp_tick(scheduler_get_uptime_ticks());
		ntp_tick(scheduler_get_uptime_ticks());
		ndp_tick(scheduler_get_uptime_ticks());
		if (local_ip.bytes[0] == 0) {
			u64 now = scheduler_get_uptime_ticks();
			if (now - last_dhcp_retry >= 300) {
				dhcp_init();
				last_dhcp_retry = now;
			}
		}
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
	memset(&local_mac, 0, sizeof(local_mac));
	local_ip = (struct ipv4_addr){{0, 0, 0, 0}};
	gateway_ip = (struct ipv4_addr){{0, 0, 0, 0}};
	net_scan_pci_adapters();

	/* NIC driver probe: the first driver to register becomes the active
	 * interface (netdev_register is first-wins). virtio-net is tried first so
	 * it stays the active NIC under QEMU; e1000 is then probed unconditionally
	 * so its hardware comes up for the M37-E1000 self-test even when virtio
	 * already won. On real hardware virtio-net is absent, so e1000 (the host's
	 * Intel I219-V and the rest of the gigabit family) becomes active. */
	virtio_net_probe();
	e1000_probe();

	struct netdev *nd = netdev_active();
	console_write("net: pci adapters 0x");
	console_write_hex64(net_adapter_count);
	console_write(", driver ");
	console_write(nd ? nd->name : "none");
	console_write("\n");

	if (nd) {
		local_mac = nd->mac;
		net_compute_link_local();
	}

	arp_init();
	ndp_init();
	if (!nd) {
		return;
	}
	/* Networking is on by default: bring the link up via DHCP whenever a NIC is
	 * present. Opt out with b1nix.net=off (or b1nix.nonet) for an isolated boot;
	 * b1nix.net=dhcp is still accepted as an explicit no-op for back-compat. */
	if (!bootinfo_has_flag("b1nix.net=off") && !bootinfo_has_flag("b1nix.nonet")) {
		dhcp_init();
	}

	net_task_id = kthread_create("net_task", net_task, 0);
}

void net_send_ethernet(struct mac_addr dst, u16 ethertype, const void *payload, usize size)
{
	struct netdev *nd = netdev_active();
	if (!nd || !nd->transmit) {
		return;
	}

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
			ipv6_receive(data, len);
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

	if (nd->poll)
		nd->poll(nd);
}
