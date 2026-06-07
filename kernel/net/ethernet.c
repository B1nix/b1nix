#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/console.h>

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV6 0x86DD

void ethernet_receive(const void *data, usize size)
{
	/* Drivers are all polled so their RX/TX rings keep moving, but the current
	 * stack has one L3 address context. Do not let traffic from a standby NIC
	 * poison ARP/DHCP state or produce replies on the active NIC. */
	struct netdev *rx = netdev_receiving();
	if (rx && rx != netdev_active())
		return;

	if (size < 14) return;

	const u8 *header = data;
	u16 ethertype = (header[12] << 8) | header[13];

	const void *payload = header + 14;
	usize payload_size = size - 14;

	if (ethertype == ETHERTYPE_ARP) {
		arp_receive(payload, payload_size);
	} else if (ethertype == ETHERTYPE_IPV4) {
		ipv4_receive(payload, payload_size);
	} else if (ethertype == ETHERTYPE_IPV6) {
		ipv6_receive(payload, payload_size);
	}
}
