#include <b1nix/console.h>
#include <b1nix/net.h>
#include <b1nix/pci.h>
#include <b1nix/virtio.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/arch_x86.h>
#include <string.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_NET_DEVICE_ID 0x1000

static struct virtio_device net_dev;
static struct virtqueue net_rx_vq;
static struct virtqueue net_tx_vq;
static volatile int net_ready;

static volatile int net_tx_lock = 0;
static volatile int net_rx_lock = 0;
static volatile int net_irq_pending = 0;
static void **tx_buffers;
static u8 *tx_inflight;

static struct mac_addr local_mac;
static struct ipv4_addr local_ip = { { 0, 0, 0, 0 } };
static struct ipv4_addr gateway_ip = { { 0, 0, 0, 0 } };

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
int net_is_ready(void) { return net_ready; }
int net_get_irq(void) { return net_ready ? net_dev.irq : -1; }

void net_interrupt_handler(void) {
	if (!net_ready) return;
	if (virtio_read_isr(&net_dev) & 1) {
		net_irq_pending = 1;
	}
}

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
	console_write("Network\n");
	console_write(" driver: ");
	console_write(net_ready ? "virtio-net" : "none");
	console_write("\n link:   ");
	console_write(net_ready ? "up" : "down");
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

#define RX_BUFFER_SIZE 2048

struct rx_buffer {
	struct virtio_net_hdr hdr;
	u8 data[RX_BUFFER_SIZE];
} __attribute__((packed));

static struct rx_buffer *rx_buffers;
static u16 rx_buffer_count;

static int is_power_of_two_u16(u16 v)
{
	return v && ((v & (u16)(v - 1)) == 0);
}

static void fill_rx_buffer(u16 idx)
{
	u16 d0 = idx;
	net_rx_vq.desc[d0].addr = (u64)(usize)&rx_buffers[idx];
	net_rx_vq.desc[d0].len = sizeof(struct rx_buffer);
	net_rx_vq.desc[d0].flags = VRING_DESC_F_WRITE;
	net_rx_vq.desc[d0].next = 0;

	u16 avail_idx = net_rx_vq.avail->idx % net_rx_vq.queue_size;
	net_rx_vq.avail->ring[avail_idx] = d0;

	__asm__ volatile("" ::: "memory");
	net_rx_vq.avail->idx++;
	__asm__ volatile("" ::: "memory");
}

static void virtio_net_probe(void)
{
	if (!virtio_init_device(&net_dev, VIRTIO_VENDOR_ID, VIRTIO_NET_DEVICE_ID)) {
		console_write("virtio-net: no device found\n");
		net_ready = 0;
		return;
	}

	virtio_set_guest_features(&net_dev, 0);
	virtio_set_status(&net_dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

	if (!virtq_init(&net_dev, 0, &net_rx_vq)) {
		virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_FAILED);
		return;
	}
	if (!virtq_init(&net_dev, 1, &net_tx_vq)) {
		virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_FAILED);
		return;
	}
	if (net_rx_vq.queue_size == 0 || net_tx_vq.queue_size == 0 ||
	    !is_power_of_two_u16(net_rx_vq.queue_size) ||
	    !is_power_of_two_u16(net_tx_vq.queue_size)) {
		console_write("virtio-net: invalid virtqueue size (must be power-of-two)\n");
		virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_FAILED);
		return;
	}
	tx_buffers = kzalloc(sizeof(void *) * net_tx_vq.queue_size);
	tx_inflight = kzalloc(sizeof(u8) * net_tx_vq.queue_size);
	if (!tx_buffers || !tx_inflight) {
		virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_FAILED);
		return;
	}

	// Read MAC from device config space (ports + 20 for legacy virtio pci)
	for (int i = 0; i < 6; i++) {
		// Port I/O read from config space
		u16 port = net_dev.port_base + 20 + i;
		u8 val;
		__asm__ volatile("inb %w1, %b0" : "=a"(val) : "Nd"(port));
		local_mac.bytes[i] = val;
	}

	virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_DRIVER_OK);

	// Populate RX queue
	rx_buffer_count = net_rx_vq.queue_size;
	rx_buffers = kzalloc(sizeof(struct rx_buffer) * rx_buffer_count);
	for (u16 i = 0; i < rx_buffer_count; i++) {
		fill_rx_buffer(i);
	}
	virtq_kick(&net_dev, &net_rx_vq);
	net_ready = 1;

	console_write("virtio-net: initialized with MAC ");
	print_mac(local_mac);
	console_write("\n");

	if (net_dev.irq != 0xFF) {
		x86_pic_unmask(net_dev.irq);
	}
}

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
		if (local_ip.bytes[0] == 0) {
			u64 now = scheduler_get_uptime_ticks();
			if (now - last_dhcp_retry >= 300) {
				dhcp_init();
				last_dhcp_retry = now;
			}
		}
		scheduler_yield();
	}
}

void net_init(void)
{
	net_ready = 0;
	memset(&local_mac, 0, sizeof(local_mac));
	local_ip = (struct ipv4_addr){{0, 0, 0, 0}};
	gateway_ip = (struct ipv4_addr){{0, 0, 0, 0}};
	net_scan_pci_adapters();
	virtio_net_probe();
	console_write("net: pci adapters 0x");
	console_write_hex64(net_adapter_count);
	console_write(", driver ");
	console_write(net_ready ? "virtio-net\n" : "none\n");
	arp_init();
	if (!net_ready) {
		return;
	}
	dhcp_init();

	kthread_create("net_task", net_task, 0);
}

void net_send_ethernet(struct mac_addr dst, u16 ethertype, const void *payload, usize size)
{
	if (!net_ready || net_tx_vq.queue_size == 0 || !net_tx_vq.desc || !net_tx_vq.avail || !net_tx_vq.used) {
		return;
	}

	// Allocate TX buffer. It is reclaimed on TX completion in net_poll().
	usize packet_size = sizeof(struct virtio_net_hdr) + 14 + size;
	u8 *buffer = kzalloc(packet_size);
	if (!buffer) return;

	struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)buffer;
	hdr->flags = 0;
	hdr->gso_type = 0;

	u8 *eth_hdr = buffer + sizeof(struct virtio_net_hdr);
	memcpy(eth_hdr, dst.bytes, 6);
	memcpy(eth_hdr + 6, local_mac.bytes, 6);
	eth_hdr[12] = (ethertype >> 8) & 0xFF;
	eth_hdr[13] = ethertype & 0xFF;
	memcpy(eth_hdr + 14, payload, size);

	for (int tries = 0; tries < 2; tries++) {
		while (__atomic_test_and_set(&net_tx_lock, __ATOMIC_ACQUIRE)) scheduler_yield();

		u16 d0 = 0xFFFF;
		for (u16 i = 0; i < net_tx_vq.queue_size; i++) {
			if (!tx_inflight[i]) {
				d0 = i;
				break;
			}
		}
		if (d0 == 0xFFFF) {
			__atomic_clear(&net_tx_lock, __ATOMIC_RELEASE);
			net_poll();
			continue;
		}

		tx_buffers[d0] = buffer;
		tx_inflight[d0] = 1;
		net_tx_vq.desc[d0].addr = (u64)(usize)buffer;
		net_tx_vq.desc[d0].len = packet_size;
		net_tx_vq.desc[d0].flags = 0;
		net_tx_vq.desc[d0].next = 0;

		u16 avail_idx = net_tx_vq.avail->idx % net_tx_vq.queue_size;
		net_tx_vq.avail->ring[avail_idx] = d0;
		__asm__ volatile("" ::: "memory");
		net_tx_vq.avail->idx++;
		__asm__ volatile("" ::: "memory");
		virtq_kick(&net_dev, &net_tx_vq);
		__atomic_clear(&net_tx_lock, __ATOMIC_RELEASE);
		return;
	}

	kfree(buffer);
}

void net_poll(void)
{
	if (!net_ready || net_rx_vq.queue_size == 0 || !net_rx_vq.used) {
		return;
	}

	tcp_timer_tick();

	if (__atomic_test_and_set(&net_tx_lock, __ATOMIC_ACQUIRE)) {
		return;
	}
	while (net_tx_vq.used && net_tx_vq.used->idx != net_tx_vq.last_used_idx) {
		u16 used_idx = net_tx_vq.last_used_idx % net_tx_vq.queue_size;
		u32 id = net_tx_vq.used->ring[used_idx].id;
		if (id < net_tx_vq.queue_size && tx_inflight[id]) {
			tx_inflight[id] = 0;
			kfree(tx_buffers[id]);
			tx_buffers[id] = 0;
		}
		net_tx_vq.last_used_idx++;
	}
	__atomic_clear(&net_tx_lock, __ATOMIC_RELEASE);

	if (__atomic_test_and_set(&net_rx_lock, __ATOMIC_ACQUIRE)) return; // Don't block if someone else is already polling

	while (net_rx_vq.used->idx != net_rx_vq.last_used_idx) {
		u16 used_idx = net_rx_vq.last_used_idx % net_rx_vq.queue_size;
		u32 id = net_rx_vq.used->ring[used_idx].id;
		u32 len = net_rx_vq.used->ring[used_idx].len;

		if (id < rx_buffer_count) {
			struct rx_buffer *buf = &rx_buffers[id];
			if (len > sizeof(struct virtio_net_hdr)) {
				usize payload_len = len - sizeof(struct virtio_net_hdr);
				ethernet_receive(buf->data, payload_len);
			}

			// Re-arm
			fill_rx_buffer((u16)id);
			virtq_kick(&net_dev, &net_rx_vq);
		}

		net_rx_vq.last_used_idx++;
	}

	__atomic_clear(&net_rx_lock, __ATOMIC_RELEASE);
}
