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

/*
 * What a device is, which is what tells the stack whether a frame arriving on
 * it needs anything doing before the protocol demux (see ethernet_receive) and
 * whether the interface may be picked to carry the L3 configuration. A
 * physical NIC is the default; everything else is created on request and
 * stacked on top of one.
 */
enum netdev_kind {
	NETDEV_KIND_PHYS = 0,   /* a real NIC, registered by its driver */
	NETDEV_KIND_VLAN,       /* 802.1Q tag on transmit, match+strip on receive */
	NETDEV_KIND_BRIDGE,     /* software bridge with a learning FDB */
	NETDEV_KIND_BOND,       /* active-backup aggregation of two or more NICs */
	NETDEV_KIND_GRETAP,     /* ethernet over GRE over IPv4 */
	NETDEV_KIND_VETH,       /* one end of a pair; its peer receives what it sends */
};

struct netdev {
	const char *name;          /* "virtio-net", "e1000", ...           */
	/*
	 * The interface name userspace sees ("eth0", "br0", "vlan10"). A driver
	 * leaves it empty and netdev_register() assigns the next eth<N>; a virtual
	 * device is created with the name the operator asked for.
	 */
	char ifname[16];
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

	/* What kind of device this is (enum netdev_kind). */
	u8 kind;

	/*
	 * Stacking. `lower` is the device a VLAN or tunnel sends through; `master`
	 * is the bridge or bond this device has been enslaved to. A frame that
	 * arrives on an enslaved device belongs to its master, which is what
	 * ->rx_from_port is called with — the same shape as Linux's rx_handler.
	 */
	struct netdev *lower;
	struct netdev *master;
	void (*rx_from_port)(struct netdev *master, struct netdev *port,
	                     const void *frame, usize len, u32 rx_flags);

	/*
	 * The network namespace this interface belongs to (0 = the initial one).
	 * Set from the creator's namespace at registration and changed only by
	 * `ip link set <dev> netns <pid>`. Every enumeration userspace can reach —
	 * netdev_by_index, netdev_index_by_name, the netlink link dump, the SIOCGIF*
	 * ioctls, /proc/net/dev — filters on it, so an interface in one namespace
	 * is not merely hidden from another, it cannot be named at all.
	 */
	u32 netns;

	/* Tear-down for a device created at runtime (NULL for a driver's NIC). */
	void (*destroy)(struct netdev *nd);

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

/* Drop a device created at runtime out of the registry. The index it held is
 * retired rather than reused by the next device: a route or a socket that
 * recorded that ifindex must not silently start naming a different
 * interface. */
void netdev_unregister(struct netdev *nd);

/* 1 when the device is something the stack made rather than something a driver
 * probed. Such a device is never auto-selected to carry the L3 configuration:
 * running DHCP on a bridge is a decision an operator makes, not a default. */
int netdev_is_virtual(const struct netdev *nd);

/*
 * Put one already-built frame on `nd` and let the packet-socket transmit tap
 * see it. Every path that hands a frame to a device short of
 * net_send_ethernet_tx() — a bridge forwarding, a VLAN tagging, a bond picking
 * its active slave — goes through here, so tcpdump on the lower device shows
 * the frame exactly as it went out. Returns the driver's verdict.
 */
int netdev_transmit_frame(struct netdev *nd, const u8 hdr[14],
                          const void *payload, usize payload_len, u32 tx_flags);

/*
 * Feed a frame into the receive path as if `dev` had just received it: the
 * packet-socket tap, the bridge/bond hand-off and the protocol demux all run
 * against `dev`. This is how a virtual device delivers what it decapsulated.
 * Re-entrant, with a small depth bound so a mis-wired stack cannot recurse
 * forever.
 */
void net_deliver_frame(struct netdev *dev, const void *frame, usize len,
                       u32 rx_flags);

/* The active NIC of the caller's network namespace, or NULL if it has none.
 * The initial namespace's answer is the device the drivers probed. */
struct netdev *netdev_active(void);
/* The same question asked about a namespace that is not the caller's — the
 * transmit path needs it to stamp a source address for the interface a frame
 * actually leaves by. */
struct netdev *netdev_active_ns(u32 ns);
/* Does this interface carry its namespace's IPv4 configuration? Destroying or
 * enslaving one that does would take the address with it. */
int netdev_holds_address(struct netdev *nd);

/* The registry without the namespace filter, for the kernel's own scans
 * (net_poll, teardown). `idx` is 1-based, as netdev_index_of returns. */
struct netdev *netdev_slot(int idx);
usize netdev_slot_count(void);

/* Move an interface into network namespace `ns`. Refuses to move the device
 * carrying the initial namespace's L3 configuration out from under it. */
int netdev_set_netns(struct netdev *nd, u32 ns);

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
