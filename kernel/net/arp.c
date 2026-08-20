#include <b1nix/namespace.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/console.h>
#include <b1nix/bootinfo.h>
#include <b1nix/errno.h>
#include <b1nix/posix.h>
#include <string.h>

#define ARP_HW_ETHERNET 1
#define ARP_PROTO_IPV4 0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2

struct arp_packet {
	u16 hw_type;
	u16 proto_type;
	u8 hw_len;
	u8 proto_len;
	u16 op;
	struct mac_addr sender_mac;
	struct ipv4_addr sender_ip;
	struct mac_addr target_mac;
	struct ipv4_addr target_ip;
} __attribute__((packed));

static u16 bswap16(u16 value)
{
	return (u16)((value << 8) | (value >> 8));
}

#define ARP_TABLE_SIZE 64
struct arp_entry {
	struct ipv4_addr ip;
	struct mac_addr mac;
	int valid;
	/* M107: set for an entry installed administratively (`ip neigh add`). A
	 * learned reply never overwrites one, and it reports NUD_PERMANENT. */
	int permanent;
	int oif; /* interface the mapping was learned on, 0 = unknown */
	/* The network namespace the mapping belongs to. Two namespaces may use the
	 * same address for different machines, so an entry learned in one must
	 * never answer a lookup made in another. */
	u32 ns;
};
static struct arp_entry arp_table[ARP_TABLE_SIZE];
static int arp_smoke_resolution_logged;
static int arp_smoke_request_logged;
static int arp_smoke_reply_logged;

/* Which namespace is asking: the arriving interface's inside a receive path,
 * the caller's otherwise — the same rule the FIB uses. */
static u32 arp_ns(void) { return namespace_net_context(); }

static void arp_smoke_mark_resolution(void)
{
	if (bootinfo_has_flag("b1nix.test=1") && !arp_smoke_resolution_logged) {
		arp_smoke_resolution_logged = 1;
		console_write("\nARP-SMOKE: resolution-ready\n");
	}
}

void arp_init(void)
{
	arp_smoke_resolution_logged = 0;
	arp_smoke_request_logged = 0;
	arp_smoke_reply_logged = 0;
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		arp_table[i].valid = 0;
		arp_table[i].permanent = 0;
		arp_table[i].oif = 0;
		arp_table[i].ns = 0;
	}
}

/* Everything a namespace that is going away had learned. */
void arp_flush_ns(u32 ns)
{
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].ns != ns)
			continue;
		arp_table[i].valid = 0;
		arp_table[i].permanent = 0;
		arp_table[i].oif = 0;
		arp_table[i].ns = 0;
	}
}

/* The interface a mapping belongs to: the one currently delivering frames when
 * we are inside a receive path, else the active NIC. */
static int arp_current_oif(void)
{
	struct netdev *nd = netdev_receiving();
	if (!nd)
		nd = netdev_active();
	return netdev_index_of(nd);
}

static void arp_cache_put(struct ipv4_addr ip, struct mac_addr mac)
{
	u32 ns = arp_ns();
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && arp_table[i].ns == ns &&
		    memcmp(arp_table[i].ip.bytes, ip.bytes, 4) == 0) {
			/* An administratively pinned entry outranks the wire. */
			if (arp_table[i].permanent)
				return;
			arp_table[i].mac = mac;
			arp_table[i].oif = arp_current_oif();
			return;
		}
	}
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (!arp_table[i].valid) {
			arp_table[i].ip = ip;
			arp_table[i].mac = mac;
			arp_table[i].valid = 1;
			arp_table[i].permanent = 0;
			arp_table[i].oif = arp_current_oif();
			arp_table[i].ns = ns;
			return;
		}
	}
}

/* ── M107: neighbour-table administration (rtnetlink RTM_*NEIGH, `ip neigh`) ── */

usize arp_snapshot(struct neigh_info *out, usize max)
{
	usize n = 0;
	u32 ns = arp_ns();
	for (int i = 0; i < ARP_TABLE_SIZE && n < max; i++) {
		if (!arp_table[i].valid || arp_table[i].ns != ns)
			continue;
		out[n].family = B1NIX_AF_INET;
		memset(out[n].addr, 0, sizeof(out[n].addr));
		memcpy(out[n].addr, arp_table[i].ip.bytes, 4);
		out[n].addr_len = 4;
		out[n].permanent = (u8)(arp_table[i].permanent ? 1 : 0);
		out[n].mac = arp_table[i].mac;
		out[n].oif = arp_table[i].oif;
		n++;
	}
	return n;
}

int arp_neigh_set(struct ipv4_addr ip, struct mac_addr mac, int permanent)
{
	int oif = arp_current_oif();
	u32 ns = arp_ns();
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && arp_table[i].ns == ns &&
		    memcmp(arp_table[i].ip.bytes, ip.bytes, 4) == 0) {
			arp_table[i].mac = mac;
			arp_table[i].permanent = permanent ? 1 : 0;
			arp_table[i].oif = oif;
			return 0;
		}
	}
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (!arp_table[i].valid) {
			arp_table[i].ip = ip;
			arp_table[i].mac = mac;
			arp_table[i].valid = 1;
			arp_table[i].permanent = permanent ? 1 : 0;
			arp_table[i].oif = oif;
			arp_table[i].ns = ns;
			return 0;
		}
	}
	return -ENOSPC;
}

int arp_neigh_del(struct ipv4_addr ip)
{
	u32 ns = arp_ns();
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && arp_table[i].ns == ns &&
		    memcmp(arp_table[i].ip.bytes, ip.bytes, 4) == 0) {
			arp_table[i].valid = 0;
			arp_table[i].permanent = 0;
			arp_table[i].oif = 0;
			arp_table[i].ns = 0;
			return 0;
		}
	}
	return -ESRCH;
}

int arp_resolve_dev(struct ipv4_addr ip, struct mac_addr *mac, struct netdev *dev)
{
	int found = 0;
	u32 ns = arp_ns();
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && arp_table[i].ns == ns &&
		    memcmp(arp_table[i].ip.bytes, ip.bytes, 4) == 0) {
			*mac = arp_table[i].mac;
			found = 1;
			break;
		}
	}

	int smoke_probe = bootinfo_has_flag("b1nix.test=1") &&
	                  !arp_smoke_request_logged;
	if (!found || smoke_probe) {
		// Send ARP request
		struct arp_packet req;
		req.hw_type = bswap16(ARP_HW_ETHERNET);
		req.proto_type = bswap16(ARP_PROTO_IPV4);
		req.hw_len = 6;
		req.proto_len = 4;
		req.op = bswap16(ARP_OP_REQUEST);
		req.sender_mac = net_get_mac();
		req.sender_ip = net_get_ip();
		memset(req.target_mac.bytes, 0, 6);
		req.target_ip = ip;

		struct mac_addr bcast = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
		/* M84: broadcast the request out of the interface the route picked,
		 * not blindly out of the active one. */
		if (dev)
			req.sender_mac = dev->mac;
		net_send_ethernet_dev(dev, bcast, 0x0806, &req, sizeof(req));
		
			if (bootinfo_has_flag("b1nix.test=1") && !arp_smoke_request_logged) {
			console_write("\nARP-SMOKE: request-sent\n");
			arp_smoke_request_logged = 1;
		}
	}

	if (found) {
		arp_smoke_mark_resolution();
		return 1;
	}

	// We don't block here. Returning 0 means not found yet. The upper layer should retry later.
	return 0;
}

int arp_resolve(struct ipv4_addr ip, struct mac_addr *mac)
{
	return arp_resolve_dev(ip, mac, 0);
}

void arp_receive(const void *data, usize size)
{
	if (size < sizeof(struct arp_packet)) return;
	const struct arp_packet *pkt = data;

	if (bswap16(pkt->hw_type) != ARP_HW_ETHERNET || bswap16(pkt->proto_type) != ARP_PROTO_IPV4) return;

	arp_cache_put(pkt->sender_ip, pkt->sender_mac);
	arp_smoke_mark_resolution();
	
	if (bswap16(pkt->op) == ARP_OP_REPLY) {
		if (bootinfo_has_flag("b1nix.test=1") && !arp_smoke_reply_logged) {
			console_write("\nARP-SMOKE: reply-received\n");
			arp_smoke_reply_logged = 1;
		}
	}

	if (bswap16(pkt->op) == ARP_OP_REQUEST) {
		struct ipv4_addr my_ip = net_get_ip();
		if (memcmp(pkt->target_ip.bytes, my_ip.bytes, 4) == 0 && my_ip.bytes[0] != 0) {
			struct arp_packet reply;
			reply.hw_type = bswap16(ARP_HW_ETHERNET);
			reply.proto_type = bswap16(ARP_PROTO_IPV4);
			reply.hw_len = 6;
			reply.proto_len = 4;
			reply.op = bswap16(ARP_OP_REPLY);
			reply.sender_mac = net_get_mac();
			reply.sender_ip = my_ip;
			reply.target_mac = pkt->sender_mac;
			reply.target_ip = pkt->sender_ip;

			net_send_ethernet(pkt->sender_mac, 0x0806, &reply, sizeof(reply));
		}
	}
}
