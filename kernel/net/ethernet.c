#include <b1nix/net.h>
#include <b1nix/netproto.h>
#include <b1nix/netdev.h>
#include <b1nix/packet.h>
#include <b1nix/vnet.h>
#include <b1nix/console.h>

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV6 0x86DD
#define ETHERTYPE_VLAN 0x8100

void ethernet_receive(const void *data, usize size)
{
	ethernet_receive_flags(data, size, 0);
}

void ethernet_receive_flags(const void *data, usize size, u32 rx_flags)
{
	struct netdev *rx = netdev_receiving();
	/* An administratively down interface accepts nothing, even though its ring
	 * keeps being drained so the hardware does not wedge. */
	if (rx && !netdev_is_admin_up(rx))
		return;

	if (size < 14) return;

	/* AF_PACKET sees the frame before the protocol demux does — including the
	 * ethertypes nothing in the stack handles, which is the whole point of a
	 * packet socket. It sees it on *every* interface, physical or virtual, and
	 * before any of the decisions below, which is what makes tcpdump on a
	 * bridge port and on the bridge itself show different things. Delivery is a
	 * copy into each interested socket's queue, so the frame below is untouched
	 * by it. */
	packet_socket_rx(rx, data, size);

	const u8 *header = data;
	u16 ethertype = (header[12] << 8) | header[13];

	if (rx) {
		/* An enslaved interface does not decide anything itself: the frame
		 * belongs to its bridge or bond, which forwards it, drops it, or hands
		 * it back through net_deliver_frame() as the master's own. */
		if (rx->master && rx->master->rx_from_port) {
			rx->master->rx_from_port(rx->master, rx, data, size, rx_flags);
			return;
		}
		/* A tagged frame is the VLAN device's, not this interface's. */
		if (ethertype == ETHERTYPE_VLAN &&
		    vlan_rx(rx, data, size, rx_flags))
			return;
	}

	/* Drivers are all polled so their RX/TX rings keep moving, but the current
	 * stack has one L3 address context. Do not let traffic from a standby NIC
	 * poison ARP/DHCP state or produce replies on the active NIC. */
	if (rx && rx != netdev_active())
		return;

	const void *payload = header + 14;
	usize payload_size = size - 14;

	if (ethertype == ETHERTYPE_ARP) {
		arp_receive(payload, payload_size);
	} else if (ethertype == ETHERTYPE_IPV4) {
		ipv4_receive_flags(payload, payload_size, rx_flags);
	} else if (ethertype == ETHERTYPE_IPV6) {
		/* M96: served by ipv6.ko when it is loaded. */
		proto_deliver_ether(ETHERTYPE_IPV6, payload, payload_size);
	}
}
