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

/* virtio-net feature bits (legacy PCI, low 32 bits). */
#define VIRTIO_NET_F_CSUM        (1u << 0)   /* device checksums on transmit */
#define VIRTIO_NET_F_GUEST_CSUM  (1u << 1)   /* device validates on receive  */
#define VIRTIO_NET_F_MAC         (1u << 5)   /* config space carries the MAC */
#define VIRTIO_NET_F_MRG_RXBUF   (1u << 15)  /* 12-byte header, merged bufs  */

/* virtio_net_hdr.flags */
#define VIRTIO_NET_HDR_F_NEEDS_CSUM 0x1
#define VIRTIO_NET_HDR_F_DATA_VALID 0x2

/*
 * Everything this driver knows how to honour. Only the intersection with what
 * the host offers is ever written back, and each accepted bit changes the data
 * path in a way the code below actually implements:
 *
 *   CSUM        transmit may hand the device a partial checksum
 *   GUEST_CSUM  a received frame may be reported as already validated
 *   MAC         the MAC in config space is meaningful
 *
 * MRG_RXBUF is deliberately NOT requested: it widens the header to 12 bytes and
 * hands back multi-buffer frames, and this driver's RX path is single-buffer.
 */
#define VNET_SUPPORTED_FEATURES \
	(VIRTIO_NET_F_CSUM | VIRTIO_NET_F_GUEST_CSUM | VIRTIO_NET_F_MAC)

static struct virtio_device net_dev;
static struct virtqueue net_rx_vq;
static struct virtqueue net_tx_vq;
static volatile int vnet_ready;

static u32 vnet_features;             /* what the host and driver agreed on */
static usize vnet_hdr_size = sizeof(struct virtio_net_hdr);

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

/*
 * Ask the device to finish the TCP/UDP checksum of an outgoing IPv4 frame.
 *
 * Only legal when VIRTIO_NET_F_CSUM was negotiated AND the stack agreed to emit
 * partial checksums (net_tx_csum_offload_enabled()): under NEEDS_CSUM the
 * checksum field must hold the pseudo-header sum, so if the sender had computed
 * a full checksum instead, the device would fold it in a second time and put a
 * wrong value on the wire. Anything not recognised here (IPv6, ICMP, a
 * fragment, a truncated header) is left exactly as the stack built it, which is
 * a complete software checksum.
 */
static void vnet_tx_offload(struct virtio_net_hdr *vhdr, const u8 *frame,
                            usize frame_len, u32 tx_flags)
{
	if (!(tx_flags & NETDEV_TX_F_PARTIAL_CSUM))
		return;
	if (!(vnet_features & VIRTIO_NET_F_CSUM))
		return;
	if (frame_len < 14 + 20)
		return;
	if ((((u16)frame[12] << 8) | frame[13]) != 0x0800)
		return;

	const u8 *ip = frame + 14;
	if ((ip[0] >> 4) != 4)
		return;
	usize ihl = (usize)(ip[0] & 0x0F) * 4;
	if (ihl < 20 || 14 + ihl > frame_len)
		return;
	if ((((u16)ip[6] << 8 | ip[7]) & 0x3FFF) != 0)
		return;              /* a fragment carries no complete L4 header */

	u16 csum_offset;
	if (ip[9] == 6)
		csum_offset = 16;    /* TCP checksum field */
	else if (ip[9] == 17)
		csum_offset = 6;     /* UDP checksum field */
	else
		return;

	if (14 + ihl + csum_offset + 2 > frame_len)
		return;

	vhdr->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
	vhdr->csum_start = (u16)(14 + ihl);
	vhdr->csum_offset = csum_offset;
}

/* ── netdev ops ─────────────────────────────────────────────────────────── */

static int vnet_transmit(struct netdev *nd, const u8 hdr[14],
                         const void *payload, usize payload_len, u32 tx_flags)
{
	(void)nd;
	if (!vnet_ready || net_tx_vq.queue_size == 0 || !net_tx_vq.desc ||
	    !net_tx_vq.avail || !net_tx_vq.used) {
		return -1;
	}

	usize packet_size = vnet_hdr_size + 14 + payload_len;
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

	/* Only the virtio header has to be cleared: the frame that follows it is
	 * fully overwritten by the two memcpy()s below, and the descriptor length
	 * stops the device from ever looking past it. Zeroing the whole 4 KiB page
	 * for a 42-byte ARP request was pure waste. vnet_hdr_size covers the
	 * 12-byte MRG_RXBUF layout as well as the plain 10-byte one. */
	memset(buffer, 0, vnet_hdr_size);

	struct virtio_net_hdr *vhdr = (struct virtio_net_hdr *)buffer;

	u8 *eth_hdr = buffer + vnet_hdr_size;
	memcpy(eth_hdr, hdr, 14);
	memcpy(eth_hdr + 14, payload, payload_len);
	vnet_tx_offload(vhdr, eth_hdr, 14 + payload_len, tx_flags);

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
	/* Bound each drain to queue_size iterations: a malicious/buggy device that
	 * keeps used->idx perpetually ahead of last_used_idx would otherwise spin
	 * here forever holding net_tx_lock (R4-5). One poll never has more than
	 * queue_size genuine completions. */
	u32 tx_drained = 0;
	while (net_tx_vq.used && net_tx_vq.used->idx != net_tx_vq.last_used_idx &&
	       tx_drained < net_tx_vq.queue_size) {
		tx_drained++;
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

	u32 rx_drained = 0;
	while (net_rx_vq.used->idx != net_rx_vq.last_used_idx &&
	       rx_drained < net_rx_vq.queue_size) {
		rx_drained++;
		u16 used_idx = net_rx_vq.last_used_idx % net_rx_vq.queue_size;
		u32 id = net_rx_vq.used->ring[used_idx].id;
		u32 len = net_rx_vq.used->ring[used_idx].len;

		if (id < rx_buffer_count) {
			struct rx_buffer *buf = rx_buffers[id];
			if (len > vnet_hdr_size) {
				usize payload_len = len - vnet_hdr_size;
				if (payload_len > sizeof(buf->data)) {
					payload_len = sizeof(buf->data);
				}
				/* With GUEST_CSUM the device tells us, per frame, what state
				 * the L4 checksum is in. DATA_VALID means it verified the sum
				 * itself. NEEDS_CSUM means the opposite of what its name
				 * suggests on this side: the sum in the field is only partial,
				 * because the sender is on this host and never finished it — so
				 * there is nothing to verify and verifying anyway would drop
				 * every such frame. Everything else is checked in software by
				 * ipv4_receive_flags(). */
				u32 rx_flags = 0;
				if ((vnet_features & VIRTIO_NET_F_GUEST_CSUM) &&
				    (buf->hdr.flags & (VIRTIO_NET_HDR_F_DATA_VALID |
				                       VIRTIO_NET_HDR_F_NEEDS_CSUM)))
					rx_flags |= NET_RX_F_CSUM_OK;
				ethernet_receive_flags(buf->data, payload_len, rx_flags);
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

static int vnet_link_up(struct netdev *nd)
{
	(void)nd;
	return vnet_ready ? 1 : 0;
}

/* ── probe ──────────────────────────────────────────────────────────────── */

int virtio_net_probe(void)
{
	if (!virtio_init_device(&net_dev, VIRTIO_VENDOR_ID, VIRTIO_NET_DEVICE_ID)) {
		console_write("virtio-net: no device found\n");
		vnet_ready = 0;
		return 0;
	}

	/* Honest feature negotiation: take what the host offers, keep only the
	 * bits this driver implements, and write that intersection back. Writing a
	 * bit the host never offered is a protocol violation; writing zero (what
	 * this driver used to do) throws away every offload the host had on the
	 * table. */
	u32 host_features = virtio_get_host_features(&net_dev);
	vnet_features = host_features & VNET_SUPPORTED_FEATURES;
	virtio_set_guest_features(&net_dev, vnet_features);
	virtio_set_status(&net_dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
	                            VIRTIO_STATUS_FEATURES_OK);
	/* MRG_RXBUF is never requested, so the header stays at its 10-byte legacy
	 * size; the assignment keeps the dependency explicit. */
	vnet_hdr_size = (vnet_features & VIRTIO_NET_F_MRG_RXBUF)
	                        ? sizeof(struct virtio_net_hdr) + 2
	                        : sizeof(struct virtio_net_hdr);

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
	vnet_netdev.link_up = vnet_link_up;
	vnet_netdev.features = 0;
	if (vnet_features & VIRTIO_NET_F_CSUM)
		vnet_netdev.features |= NETDEV_F_TX_CSUM;
	if (vnet_features & VIRTIO_NET_F_GUEST_CSUM)
		vnet_netdev.features |= NETDEV_F_RX_CSUM;
	vnet_netdev.priv = 0;
	netdev_register(&vnet_netdev);

	console_write("virtio-net: initialized with MAC ");
	print_mac(mac);
	console_write(" features 0x");
	console_write_hex64(vnet_features);
	console_write(" (host 0x");
	console_write_hex64(host_features);
	console_write(")\n");

	if (net_dev.irq != 0xFF) {
		x86_pic_unmask(net_dev.irq);
	}
	return 1;
}
