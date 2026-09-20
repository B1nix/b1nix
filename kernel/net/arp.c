/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/kprintf.h>
#include <b1nix/namespace.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
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

_Static_assert(sizeof(struct arp_packet) == 28, "arp wire size");
_Static_assert(__builtin_offsetof(struct arp_packet, sender_mac) == 8, "arp sender_mac");
_Static_assert(__builtin_offsetof(struct arp_packet, sender_ip) == 14, "arp sender_ip");
_Static_assert(__builtin_offsetof(struct arp_packet, target_mac) == 18, "arp target_mac");
_Static_assert(__builtin_offsetof(struct arp_packet, target_ip) == 24, "arp target_ip");

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
static spinlock_t arp_lock; /* see arp_store */
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
		k_info(NULL, "\nARP-SMOKE: resolution-ready");
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
	u64 flags;

	spin_lock_irqsave(&arp_lock, &flags);
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].ns != ns)
			continue;
		arp_table[i].valid = 0;
		arp_table[i].permanent = 0;
		arp_table[i].oif = 0;
		arp_table[i].ns = 0;
	}
	spin_unlock_irqrestore(&arp_lock, flags);
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

/* The table is shared by every CPU: the receive path (net_task, or a sender
 * whose veth delivers synchronously) learns entries while a sender on another
 * core looks them up. Unlocked, two CPUs could claim the same free slot and one
 * entry was lost -- the sender then waited out its 25 ticks for a resolution
 * that never came and dropped the datagram (M109 netns-ipv4-source-select,
 * once userspace ran on the secondaries). Never held across a transmit: a veth
 * delivers in the caller's context and comes straight back in here. */
static unsigned arp_evict_next;

/* Record ip -> mac in `ns`. `admin`: an `ip neigh` change, which may set or
 * clear `permanent`; otherwise a learned mapping, which never overrides a
 * pinned one. With the table full, a learned entry replaces a non-permanent
 * one (round robin) rather than being dropped. Returns 0 or -ENOSPC. */
static int arp_store(u32 ns, struct ipv4_addr ip, struct mac_addr mac, int oif,
                     int admin, int permanent)
{
	u64 flags;
	int rc = 0, slot = -1;

	spin_lock_irqsave(&arp_lock, &flags);
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && arp_table[i].ns == ns &&
		    memcmp(arp_table[i].ip.bytes, ip.bytes, 4) == 0) {
			if (!admin && arp_table[i].permanent)
				goto out; /* a pinned entry outranks the wire */
			arp_table[i].mac = mac;
			arp_table[i].oif = oif;
			if (admin)
				arp_table[i].permanent = permanent ? 1 : 0;
			goto out;
		}
		if (slot < 0 && !arp_table[i].valid)
			slot = i;
	}
	for (int n = 0; slot < 0 && n < ARP_TABLE_SIZE; n++) {
		int i = (int)(arp_evict_next++ % ARP_TABLE_SIZE);

		if (!arp_table[i].permanent)
			slot = i;
	}
	if (slot < 0) {
		rc = -ENOSPC;
		goto out;
	}
	arp_table[slot].valid = 0;
	arp_table[slot].ip = ip;
	arp_table[slot].mac = mac;
	arp_table[slot].permanent = permanent ? 1 : 0;
	arp_table[slot].oif = oif;
	arp_table[slot].ns = ns;
	arp_table[slot].valid = 1;
out:
	spin_unlock_irqrestore(&arp_lock, flags);
	return rc;
}

static void arp_cache_put(struct ipv4_addr ip, struct mac_addr mac)
{
	arp_store(arp_ns(), ip, mac, arp_current_oif(), 0, 0);
}

/* ── M107: neighbour-table administration (rtnetlink RTM_*NEIGH, `ip neigh`) ── */

usize arp_snapshot(struct neigh_info *out, usize max)
{
	usize n = 0;
	u32 ns = arp_ns();
	u64 flags;

	spin_lock_irqsave(&arp_lock, &flags);
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
	spin_unlock_irqrestore(&arp_lock, flags);
	return n;
}

int arp_neigh_set(struct ipv4_addr ip, struct mac_addr mac, int permanent)
{
	return arp_store(arp_ns(), ip, mac, arp_current_oif(), 1, permanent);
}

int arp_neigh_del(struct ipv4_addr ip)
{
	u32 ns = arp_ns();
	u64 flags;
	int rc = -ESRCH;

	spin_lock_irqsave(&arp_lock, &flags);
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && arp_table[i].ns == ns &&
		    memcmp(arp_table[i].ip.bytes, ip.bytes, 4) == 0) {
			arp_table[i].valid = 0;
			arp_table[i].permanent = 0;
			arp_table[i].oif = 0;
			arp_table[i].ns = 0;
			rc = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&arp_lock, flags);
	return rc;
}

int arp_resolve_dev(struct ipv4_addr ip, struct mac_addr *mac, struct netdev *dev)
{
	int found = 0;
	u32 ns = arp_ns();
	u64 flags;

	spin_lock_irqsave(&arp_lock, &flags);
	for (int i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && arp_table[i].ns == ns &&
		    memcmp(arp_table[i].ip.bytes, ip.bytes, 4) == 0) {
			*mac = arp_table[i].mac;
			found = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&arp_lock, flags);

	int smoke_probe = bootinfo_has_flag("b1nix.test=1") &&
	                  !arp_smoke_request_logged;
	/* One request per address per 200 ms. Callers retry every tick while
	 * they wait (ipv4_send polls up to 25 times), and each retry used to put
	 * another broadcast on the wire: ~30 requests for one resolution, where
	 * one answer was all it needed. ponytail: remembers only the last address
	 * asked for; a per-entry timestamp if concurrent resolutions ever matter. */
	static struct ipv4_addr last_ip;
	static u64 last_tick;
	u64 now = scheduler_get_uptime_ticks();
	int recent = !found && last_tick &&
	             memcmp(last_ip.bytes, ip.bytes, 4) == 0 &&
	             now - last_tick < SCHED_MS_TO_TICKS(200);

	if ((!found && !recent) || smoke_probe) {
		last_ip = ip;
		last_tick = now;
		// Send ARP request
		struct arp_packet req;
		req.hw_type = bswap16(ARP_HW_ETHERNET);
		req.proto_type = bswap16(ARP_PROTO_IPV4);
		req.hw_len = 6;
		req.proto_len = 4;
		req.op = bswap16(ARP_OP_REQUEST);
		req.sender_mac = net_get_mac();
		/* The address on the target's prefix: asking from another one
		 * would teach the neighbour a mapping for the wrong address. */
		req.sender_ip = net_ipv4_source_for(ns, ip);
		memset(req.target_mac.bytes, 0, 6);
		req.target_ip = ip;

		struct mac_addr bcast = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
		/* M84: broadcast the request out of the interface the route picked,
		 * not blindly out of the active one. */
		if (dev)
			req.sender_mac = dev->mac;
		net_send_ethernet_dev(dev, bcast, 0x0806, &req, sizeof(req));
		
			if (bootinfo_has_flag("b1nix.test=1") && !arp_smoke_request_logged) {
			k_info(NULL, "\nARP-SMOKE: request-sent");
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

	/* An ARP probe (RFC 5227) is sent from 0.0.0.0 while its sender is still
	 * checking its address is free: there is no mapping in it to learn. */
	if (pkt->sender_ip.bytes[0] | pkt->sender_ip.bytes[1] |
	    pkt->sender_ip.bytes[2] | pkt->sender_ip.bytes[3])
		arp_cache_put(pkt->sender_ip, pkt->sender_mac);
	arp_smoke_mark_resolution();
	
	if (bswap16(pkt->op) == ARP_OP_REPLY) {
		if (bootinfo_has_flag("b1nix.test=1") && !arp_smoke_reply_logged) {
			k_info(NULL, "\nARP-SMOKE: reply-received");
			arp_smoke_reply_logged = 1;
		}
	}

	if (bswap16(pkt->op) == ARP_OP_REQUEST) {
		/* Any address this namespace holds is answered for, each with its
		 * own identity. */
		struct ipv4_addr my_ip = pkt->target_ip;
		if (net_ipv4_is_local_ns(arp_ns(), my_ip)) {
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
