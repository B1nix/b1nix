#ifndef B1NIX_NETDEV_H
#define B1NIX_NETDEV_H

#include <b1nix/types.h>
#include <b1nix/net.h>   /* struct mac_addr */

/*
 * Generic network-interface driver model.
 *
 * The protocol stack in kernel/net/ (ethernet/arp/ipv4/ipv6/...) is completely
 * driver-agnostic: it builds an ethernet header and a payload and hands them to
 * ->transmit, and it pumps received frames out of ->poll into
 * ethernet_receive(). Each NIC driver (virtio-net, e1000, ...) fills one of
 * these structs and calls netdev_register() once its device is fully up.
 * Registered devices are polled in parallel; one is selected as the active
 * L3 interface and DHCP can fail over to another device with carrier.
 */
struct netdev {
	const char *name;          /* "virtio-net", "e1000", ...           */
	struct mac_addr mac;       /* station address, filled by the driver */
	int irq;                   /* PCI interrupt line, or -1 if none     */

	/*
	 * Put one ethernet frame on the wire: the 14-byte header
	 * (dst|src|ethertype) in hdr[], followed by payload[0..payload_len).
	 * The driver assembles them into its own DMA buffer (and prepends any
	 * device-specific header such as the virtio_net_hdr). Returns 0 on
	 * success, <0 on error/drop. Passing the header and payload separately
	 * avoids a large intermediate frame buffer on a deep send stack.
	 *
	 * tx_flags carries per-packet transmit requests (NETDEV_TX_F_*). A driver
	 * that does not implement one simply ignores it — the packet is already
	 * complete without it, with the single exception of PARTIAL_CSUM, which the
	 * stack only ever sets when every interface advertised NETDEV_F_TX_CSUM.
	 */
	int (*transmit)(struct netdev *nd, const u8 hdr[14],
	                const void *payload, usize payload_len, u32 tx_flags);

	/*
	 * Service the device: deliver each received ethernet frame to
	 * ethernet_receive() and reclaim completed TX buffers. Called from the
	 * net_task daemon (~100 Hz) and opportunistically from the TX path.
	 */
	void (*poll)(struct netdev *nd);

	/*
	 * Acknowledge a device interrupt. Returns 1 if the interrupt was ours.
	 * Optional — may be NULL for poll-only devices.
	 */
	int (*irq_ack)(struct netdev *nd);

	/*
	 * Return 1 when carrier is present, 0 when the PHY reports link down,
	 * or -1 when the transport has no usable carrier indication.
	 */
	int (*link_up)(struct netdev *nd);

	/*
	 * Administrative state (`ip link set <if> up/down`, SIOCSIFFLAGS). Distinct
	 * from carrier: a cable can be plugged in while the operator has taken the
	 * interface down. An admin-down interface neither transmits nor accepts
	 * frames and is never chosen as the active L3 interface.
	 */
	int admin_down;

	/*
	 * Negotiated hardware offloads (NETDEV_F_*). Set by the driver before
	 * netdev_register(), and only ever from what the device actually offered:
	 * an offload that was assumed rather than negotiated corrupts every packet
	 * it touches.
	 */
	u32 features;

	void *priv;                /* driver-private state */
};

/* The device computes the TCP/UDP checksum on transmit, so a sender may leave
 * only the pseudo-header sum in the checksum field. */
#define NETDEV_F_TX_CSUM 0x1u
/* Per-packet: the L4 checksum field holds only the pseudo-header sum and the
 * device is expected to finish it. Never set unless the interface set was
 * latched as offload-capable (net_tx_csum_offload_enabled()). */
#define NETDEV_TX_F_PARTIAL_CSUM 0x1u
/* The device validates the TCP/UDP checksum on receive and reports the verdict
 * per packet; the driver passes NET_RX_F_CSUM_OK up for validated frames. */
#define NETDEV_F_RX_CSUM 0x2u



/* Register a NIC. All registered devices remain serviced by net_poll(). */
void netdev_register(struct netdev *nd);

/* The active NIC, or NULL if none was probed. */
struct netdev *netdev_active(void);

/* The NIC whose ->poll callback is currently delivering received frames. */
struct netdev *netdev_receiving(void);

/*
 * Driver probe entry points, invoked in order by net_init(). Each returns 1 if
 * it found and initialised its device (and called netdev_register), else 0.
 */
int e1000_probe(void);        /* kernel/dev/e1000.c       */
int r8169_probe(void);        /* kernel/dev/r8169.c       */
int virtio_net_probe(void);   /* kernel/dev/virtio_net.c  */

/*
 * Self-contained e1000 datapath self-test (test mode only): drives an ARP
 * request/reply exchange against the QEMU SLIRP gateway directly through the
 * e1000 ring (bypassing ethernet_receive so it never disturbs the active
 * NIC's protocol stack). Emits M37-E1000 markers. No-op if no e1000 device
 * was initialised.
 */
void e1000_selftest(void);

#endif
