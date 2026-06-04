/*
 * virtio-net (legacy, port-I/O transport) driver.
 *
 * Extracted from kernel/net/net.c when the generic netdev model landed (M37).
 * Implements struct netdev: transmit() prepends a virtio_net_hdr and submits
 * the frame on the TX virtqueue from a pre-allocated buffer pool; poll() reaps
 * completed TX buffers and delivers received frames (minus the virtio_net_hdr)
 * to ethernet_receive(); irq_ack() reads the virtio ISR.
 */
#include <b1nix/console.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/pci.h>
#include <b1nix/virtio.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/arch.h>
#include <string.h>

#define VIRTIO_VENDOR_ID     0x1AF4
#define VIRTIO_NET_DEVICE_ID 0x1000

static struct virtio_device net_dev;
static struct virtqueue net_rx_vq;
static struct virtqueue net_tx_vq;
static volatile int vnet_ready;

static volatile int net_tx_lock = 0;
static volatile int net_rx_lock = 0;

static void **tx_buffers;       /* Pre-allocated TX buffer pool */
static u8 *tx_inflight;
static u16 *tx_pool_free;       /* Stack of free buffer indices */
static u16 tx_pool_count;       /* Number of buffers free in pool */
static u16 *tx_pool_map;        /* Virtqueue desc idx -> pool idx */

#define RX_BUFFER_SIZE 2048

struct rx_buffer {
	struct virtio_net_hdr hdr;
	u8 data[RX_BUFFER_SIZE];
} __attribute__((packed));

static struct rx_buffer **rx_buffers;
static u16 rx_buffer_count;

static struct netdev vnet_netdev;

static int is_power_of_two_u16(u16 v)
{
	return v && ((v & (u16)(v - 1)) == 0);
}

static void print_mac(struct mac_addr mac)
{
	const char *digits = "0123456789abcdef";
	for (int i = 0; i < 6; i++) {
		console_putc(digits[(mac.bytes[i] >> 4) & 0xf]);
		console_putc(digits[mac.bytes[i] & 0xf]);
		if (i < 5) console_putc(':');
	}
}

static void fill_rx_buffer(u16 idx)
{
	u16 d0 = idx;
	net_rx_vq.desc[d0].addr = vmm_virt_to_phys(rx_buffers[idx]);
	net_rx_vq.desc[d0].len = sizeof(struct rx_buffer);
	net_rx_vq.desc[d0].flags = VRING_DESC_F_WRITE;
	net_rx_vq.desc[d0].next = 0;

	u16 avail_idx = net_rx_vq.avail->idx % net_rx_vq.queue_size;
	net_rx_vq.avail->ring[avail_idx] = d0;

	__asm__ volatile("" ::: "memory");
	net_rx_vq.avail->idx++;
	__asm__ volatile("" ::: "memory");
}

/* ── netdev ops ─────────────────────────────────────────────────────────── */

static int vnet_transmit(struct netdev *nd, const u8 hdr[14],
                         const void *payload, usize payload_len)
{
	(void)nd;
	if (!vnet_ready || net_tx_vq.queue_size == 0 || !net_tx_vq.desc ||
	    !net_tx_vq.avail || !net_tx_vq.used) {
		return -1;
	}

	usize packet_size = sizeof(struct virtio_net_hdr) + 14 + payload_len;
	if (packet_size > PAGE_SIZE) return -1;

	/* Grab a pre-allocated TX buffer from the pool (no IRQ-time allocation). */
	u8 *buffer = 0;
	u16 pool_idx = 0;
	for (int tries = 0; tries < 2; tries++) {
		while (__atomic_test_and_set(&net_tx_lock, __ATOMIC_ACQUIRE)) scheduler_yield();
		if (tx_pool_count > 0) {
			tx_pool_count--;
			pool_idx = tx_pool_free[tx_pool_count];
			buffer = tx_buffers[pool_idx];
			__atomic_clear(&net_tx_lock, __ATOMIC_RELEASE);
			break;
		}
		__atomic_clear(&net_tx_lock, __ATOMIC_RELEASE);
		vnet_netdev.poll(&vnet_netdev);
	}
	if (!buffer) return -1;
	memset(buffer, 0, PAGE_SIZE);

	struct virtio_net_hdr *vhdr = (struct virtio_net_hdr *)buffer;
	vhdr->flags = 0;
	vhdr->gso_type = 0;

	u8 *eth_hdr = buffer + sizeof(struct virtio_net_hdr);
	memcpy(eth_hdr, hdr, 14);
	memcpy(eth_hdr + 14, payload, payload_len);

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
			vnet_netdev.poll(&vnet_netdev);
			continue;
		}

		tx_pool_map[d0] = pool_idx;
		tx_inflight[d0] = 1;
		net_tx_vq.desc[d0].addr = vmm_virt_to_phys(buffer);
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
		return 0;
	}

	/* Return buffer to pool on send failure. */
	while (__atomic_test_and_set(&net_tx_lock, __ATOMIC_ACQUIRE)) scheduler_yield();
	tx_pool_free[tx_pool_count] = pool_idx;
	tx_pool_count++;
	__atomic_clear(&net_tx_lock, __ATOMIC_RELEASE);
	return -1;
}

static void vnet_poll(struct netdev *nd)
{
	(void)nd;
	if (!vnet_ready || net_tx_vq.queue_size == 0 || !net_rx_vq.used) {
		return;
	}

	/* Reap completed TX descriptors and return their buffers to the pool. */
	if (__atomic_test_and_set(&net_tx_lock, __ATOMIC_ACQUIRE)) {
		return;
	}
	while (net_tx_vq.used && net_tx_vq.used->idx != net_tx_vq.last_used_idx) {
		u16 used_idx = net_tx_vq.last_used_idx % net_tx_vq.queue_size;
		u32 id = net_tx_vq.used->ring[used_idx].id;
		if (id < net_tx_vq.queue_size && tx_inflight[id]) {
			tx_inflight[id] = 0;
			u16 pool_ret = tx_pool_map[id];
			if (pool_ret < net_tx_vq.queue_size) {
				tx_pool_free[tx_pool_count] = pool_ret;
				tx_pool_count++;
			}
		}
		net_tx_vq.last_used_idx++;
	}
	__atomic_clear(&net_tx_lock, __ATOMIC_RELEASE);

	/* Don't block if someone else is already polling RX. */
	if (__atomic_test_and_set(&net_rx_lock, __ATOMIC_ACQUIRE)) return;

	while (net_rx_vq.used->idx != net_rx_vq.last_used_idx) {
		u16 used_idx = net_rx_vq.last_used_idx % net_rx_vq.queue_size;
		u32 id = net_rx_vq.used->ring[used_idx].id;
		u32 len = net_rx_vq.used->ring[used_idx].len;

		if (id < rx_buffer_count) {
			struct rx_buffer *buf = rx_buffers[id];
			if (len > sizeof(struct virtio_net_hdr)) {
				usize payload_len = len - sizeof(struct virtio_net_hdr);
				if (payload_len > sizeof(buf->data)) {
					payload_len = sizeof(buf->data);
				}
				ethernet_receive(buf->data, payload_len);
			}

			/* Re-arm. */
			fill_rx_buffer((u16)id);
			virtq_kick(&net_dev, &net_rx_vq);
		}

		net_rx_vq.last_used_idx++;
	}

	__atomic_clear(&net_rx_lock, __ATOMIC_RELEASE);
}

static int vnet_irq_ack(struct netdev *nd)
{
	(void)nd;
	if (!vnet_ready) return 0;
	u8 isr = virtio_read_isr(&net_dev);
	return (isr & 1) ? 1 : 0;
}

/* ── probe ──────────────────────────────────────────────────────────────── */

int virtio_net_probe(void)
{
	if (!virtio_init_device(&net_dev, VIRTIO_VENDOR_ID, VIRTIO_NET_DEVICE_ID)) {
		console_write("virtio-net: no device found\n");
		vnet_ready = 0;
		return 0;
	}

	virtio_set_guest_features(&net_dev, 0);
	virtio_set_status(&net_dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
	                            VIRTIO_STATUS_FEATURES_OK);

	if (!virtq_init(&net_dev, 0, &net_rx_vq)) {
		virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_FAILED);
		return 0;
	}
	if (!virtq_init(&net_dev, 1, &net_tx_vq)) {
		virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_FAILED);
		return 0;
	}
	net_tx_vq.avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
	if (net_rx_vq.queue_size == 0 || net_tx_vq.queue_size == 0 ||
	    !is_power_of_two_u16(net_rx_vq.queue_size) ||
	    !is_power_of_two_u16(net_tx_vq.queue_size)) {
		console_write("virtio-net: invalid virtqueue size (must be power-of-two)\n");
		virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_FAILED);
		return 0;
	}
	tx_buffers = kzalloc(sizeof(void *) * net_tx_vq.queue_size);
	tx_inflight = kzalloc(sizeof(u8) * net_tx_vq.queue_size);
	tx_pool_free = kzalloc(sizeof(u16) * net_tx_vq.queue_size);
	tx_pool_map = kzalloc(sizeof(u16) * net_tx_vq.queue_size);
	if (!tx_buffers || !tx_inflight || !tx_pool_free || !tx_pool_map) {
		virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_FAILED);
		return 0;
	}
	/* Pre-allocate TX buffer pool. */
	tx_pool_count = net_tx_vq.queue_size;
	for (u16 i = 0; i < net_tx_vq.queue_size; i++) {
		u64 frame = pmm_alloc_frame();
		if (!frame) { tx_pool_count = i; break; }
		tx_buffers[i] = (void *)(usize)(frame + vmm_direct_map_base());
		memset(tx_buffers[i], 0, PAGE_SIZE);
		tx_pool_free[i] = i;
	}

	/* Read MAC from device config space (port_base + 20 for legacy virtio PCI). */
	struct mac_addr mac;
	for (int i = 0; i < 6; i++) {
		u16 port = net_dev.port_base + 20 + i;
		u8 val;
		__asm__ volatile("inb %w1, %b0" : "=a"(val) : "Nd"(port));
		mac.bytes[i] = val;
	}

	virtio_set_status(&net_dev, virtio_get_status(&net_dev) | VIRTIO_STATUS_DRIVER_OK);

	/* Populate RX queue. */
	rx_buffer_count = net_rx_vq.queue_size;
	rx_buffers = kzalloc(sizeof(struct rx_buffer *) * rx_buffer_count);
	for (u16 i = 0; i < rx_buffer_count; i++) {
		u64 frame = pmm_alloc_frame();
		if (!frame) {
			console_write("virtio-net: failed to allocate RX buffer frame\n");
			return 0;
		}
		rx_buffers[i] = (struct rx_buffer *)(usize)(frame + vmm_direct_map_base());
		memset(rx_buffers[i], 0, PAGE_SIZE);
		fill_rx_buffer(i);
	}

	virtq_kick(&net_dev, &net_rx_vq);
	vnet_ready = 1;

	vnet_netdev.name = "virtio-net";
	vnet_netdev.mac = mac;
	vnet_netdev.irq = (net_dev.irq == 0xFF) ? -1 : (int)net_dev.irq;
	vnet_netdev.transmit = vnet_transmit;
	vnet_netdev.poll = vnet_poll;
	vnet_netdev.irq_ack = vnet_irq_ack;
	vnet_netdev.priv = 0;
	netdev_register(&vnet_netdev);

	console_write("virtio-net: initialized with MAC ");
	print_mac(mac);
	console_write("\n");

	if (net_dev.irq != 0xFF) {
		x86_pic_unmask(net_dev.irq);
	}
	return 1;
}
