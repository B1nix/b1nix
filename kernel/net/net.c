#include <b1nix/console.h>
#include <b1nix/net.h>
#include <b1nix/virtio.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <string.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_NET_DEVICE_ID 0x1000

static struct virtio_device net_dev;
static struct virtqueue net_rx_vq;
static struct virtqueue net_tx_vq;

static struct mac_addr local_mac;
static struct ipv4_addr local_ip = { { 0, 0, 0, 0 } };
static struct ipv4_addr gateway_ip = { { 0, 0, 0, 0 } };

struct mac_addr net_get_mac(void) { return local_mac; }
struct ipv4_addr net_get_ip(void) { return local_ip; }
struct ipv4_addr net_get_gateway(void) { return gateway_ip; }
void net_set_ip(struct ipv4_addr ip) { local_ip = ip; }
void net_set_gateway(struct ipv4_addr gw) { gateway_ip = gw; }

#define RX_BUFFER_SIZE 2048
#define NUM_RX_BUFFERS 16

struct rx_buffer {
	struct virtio_net_hdr hdr;
	u8 data[RX_BUFFER_SIZE];
} __attribute__((packed));

static struct rx_buffer *rx_buffers;

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
		return;
	}

	virtio_set_guest_features(&net_dev, 0);
	virtio_set_status(&net_dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

	if (!virtq_init(&net_dev, 0, &net_rx_vq)) return;
	if (!virtq_init(&net_dev, 1, &net_tx_vq)) return;

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
	rx_buffers = kzalloc(sizeof(struct rx_buffer) * NUM_RX_BUFFERS);
	for (u16 i = 0; i < NUM_RX_BUFFERS; i++) {
		fill_rx_buffer(i);
	}
	virtq_kick(&net_dev, &net_rx_vq);

	console_write("virtio-net: initialized with MAC ");
	for (int i = 0; i < 6; i++) {
		if (local_mac.bytes[i] < 0x10) console_write("0");
		console_write_hex32(local_mac.bytes[i]);
		if (i < 5) console_write(":");
	}
	console_write("\n");
}

static void net_task(void *arg)
{
	(void)arg;
	while (1) {
		net_poll();
		scheduler_yield();
	}
}

void net_init(void)
{
	virtio_net_probe();
	arp_init();
	dhcp_init();
	
	// Wait until DHCP gets an IP or a timeout
	for (int i = 0; i < 50; i++) {
		net_poll();
		if (local_ip.bytes[0] != 0) break;
		for (volatile int j = 0; j < 1000000; j++); // Simple delay
	}

	kthread_create("net_task", net_task, 0);
}

void net_send_ethernet(struct mac_addr dst, u16 ethertype, const void *payload, usize size)
{
	// Allocate TX buffer (simplistic bump allocation for now, no free)
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

	u16 d0 = net_tx_vq.avail->idx % net_tx_vq.queue_size;
	net_tx_vq.desc[d0].addr = (u64)(usize)buffer;
	net_tx_vq.desc[d0].len = packet_size;
	net_tx_vq.desc[d0].flags = 0;
	net_tx_vq.desc[d0].next = 0;

	net_tx_vq.avail->ring[d0] = d0;
	__asm__ volatile("" ::: "memory");
	net_tx_vq.avail->idx++;
	__asm__ volatile("" ::: "memory");

	virtq_kick(&net_dev, &net_tx_vq);

	// Poll until sent
	while (net_tx_vq.used->idx == net_tx_vq.last_used_idx) {
		__asm__ volatile("pause" ::: "memory");
	}
	net_tx_vq.last_used_idx++;
}

void net_poll(void)
{
	while (net_rx_vq.used->idx != net_rx_vq.last_used_idx) {
		u16 used_idx = net_rx_vq.last_used_idx % net_rx_vq.queue_size;
		u32 id = net_rx_vq.used->ring[used_idx].id;
		u32 len = net_rx_vq.used->ring[used_idx].len;

		if (id < NUM_RX_BUFFERS) {
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
}
