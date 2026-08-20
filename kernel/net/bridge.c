/* Software ethernet bridge (M109).
 *
 * A bridge is a struct netdev with ports. What makes it a bridge rather than a
 * repeater is the forwarding database: every frame that arrives on a port
 * teaches it which port that source address lives behind, and a frame for a
 * destination it has already learned goes out that one port instead of all of
 * them. A destination it has not learned, and every broadcast or multicast, is
 * flooded to every port except the one it came in on.
 *
 * The bridge device itself is an interface like any other: frames addressed to
 * its own MAC (and every broadcast) are delivered up the stack as if they had
 * arrived on it, and anything it transmits is looked up in the same database.
 *
 * Created with `ip link add name br0 type bridge` and populated with
 * `ip link set eth1 master br0`.
 */

#include <b1nix/vnet.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <string.h>

#define BRIDGE_MAX_DEVS  4
#define BRIDGE_MAX_PORTS 8
#define BRIDGE_FDB_SIZE  64
/* Linux ages a learned address out after 300 seconds. The scheduler counts
 * ticks at 100 Hz. */
#define BRIDGE_FDB_AGEING_TICKS (300ull * 100ull)

struct bridge_fdb {
	u8 mac[6];
	u8 used;
	struct netdev *port;
	u64 stamp;
};

struct bridge_dev {
	struct netdev nd;
	struct netdev *ports[BRIDGE_MAX_PORTS];
	struct bridge_fdb fdb[BRIDGE_FDB_SIZE];
	spinlock_t lock;
	u8 used;
};

static struct bridge_dev bridges[BRIDGE_MAX_DEVS];

static struct bridge_dev *bridge_from_netdev(struct netdev *nd)
{
	return nd ? (struct bridge_dev *)nd->priv : 0;
}

static int mac_is_group(const u8 *mac)
{
	return (mac[0] & 0x01) != 0;
}

static int mac_is_zero(const u8 *mac)
{
	for (int i = 0; i < 6; i++)
		if (mac[i])
			return 0;
	return 1;
}

/* ── forwarding database ────────────────────────────────────────────────── */

static void bridge_learn(struct bridge_dev *br, const u8 *src,
                         struct netdev *port)
{
	/* A group address can never be a source, and an all-zero one is not an
	 * address at all. Learning either would poison the table. */
	if (mac_is_group(src) || mac_is_zero(src))
		return;

	u64 now = scheduler_get_uptime_ticks();
	u64 flags;
	spin_lock_irqsave(&br->lock, &flags);
	struct bridge_fdb *victim = 0;
	for (int i = 0; i < BRIDGE_FDB_SIZE; i++) {
		struct bridge_fdb *e = &br->fdb[i];
		if (e->used && memcmp(e->mac, src, 6) == 0) {
			/* The station moved, or simply spoke again. */
			e->port = port;
			e->stamp = now;
			spin_unlock_irqrestore(&br->lock, flags);
			return;
		}
		if (!e->used && !victim)
			victim = e;
	}
	if (!victim) {
		/* Table full: replace the entry nothing has spoken from for longest. */
		victim = &br->fdb[0];
		for (int i = 1; i < BRIDGE_FDB_SIZE; i++)
			if (br->fdb[i].stamp < victim->stamp)
				victim = &br->fdb[i];
	}
	memcpy(victim->mac, src, 6);
	victim->port = port;
	victim->stamp = now;
	victim->used = 1;
	spin_unlock_irqrestore(&br->lock, flags);
}

/* The port `dst` was last seen behind, or NULL when it has not been learned
 * (or the entry has aged out). */
static struct netdev *bridge_lookup(struct bridge_dev *br, const u8 *dst)
{
	u64 now = scheduler_get_uptime_ticks();
	struct netdev *port = 0;
	u64 flags;
	spin_lock_irqsave(&br->lock, &flags);
	for (int i = 0; i < BRIDGE_FDB_SIZE; i++) {
		struct bridge_fdb *e = &br->fdb[i];
		if (!e->used || memcmp(e->mac, dst, 6) != 0)
			continue;
		if (now - e->stamp > BRIDGE_FDB_AGEING_TICKS) {
			e->used = 0;
			break;
		}
		port = e->port;
		break;
	}
	spin_unlock_irqrestore(&br->lock, flags);
	return port;
}

static void bridge_forget_port(struct bridge_dev *br, struct netdev *port)
{
	u64 flags;
	spin_lock_irqsave(&br->lock, &flags);
	for (int i = 0; i < BRIDGE_FDB_SIZE; i++)
		if (br->fdb[i].used && br->fdb[i].port == port)
			br->fdb[i].used = 0;
	spin_unlock_irqrestore(&br->lock, flags);
}

/* ── forwarding ─────────────────────────────────────────────────────────── */

static void bridge_send_port(struct netdev *port, const u8 *frame, usize len)
{
	if (!port || !netdev_is_admin_up(port))
		return;
	(void)netdev_transmit_frame(port, frame, frame + 14, len - 14, 0);
}

static void bridge_flood(struct bridge_dev *br, struct netdev *ingress,
                         const u8 *frame, usize len)
{
	for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
		struct netdev *p = br->ports[i];
		if (!p || p == ingress)
			continue;
		bridge_send_port(p, frame, len);
	}
}

/* A frame arrived on one of the bridge's ports: learn from it, then decide
 * where it goes. This is Linux's br_handle_frame, minus spanning tree. */
static void bridge_rx_from_port(struct netdev *master, struct netdev *port,
                                const void *frame, usize len, u32 rx_flags)
{
	struct bridge_dev *br = bridge_from_netdev(master);
	if (!br || len < 14)
		return;
	if (!netdev_is_admin_up(master))
		return;
	const u8 *f = (const u8 *)frame;

	bridge_learn(br, f + 6, port);

	if (mac_is_group(f)) {
		/* Broadcast and multicast go everywhere, including up the stack. */
		bridge_flood(br, port, f, len);
		net_deliver_frame(master, f, len, rx_flags);
		return;
	}
	if (memcmp(f, master->mac.bytes, 6) == 0) {
		net_deliver_frame(master, f, len, rx_flags);
		return;
	}
	struct netdev *out = bridge_lookup(br, f);
	if (!out) {
		bridge_flood(br, port, f, len); /* unknown unicast */
		return;
	}
	/* Already on the right side of the bridge: forwarding it back out the
	 * port it arrived on is how a bridge builds a loop. */
	if (out == port)
		return;
	bridge_send_port(out, f, len);
}

/* Frames the bridge device itself originates. */
static int bridge_transmit(struct netdev *nd, const u8 hdr[14],
                           const void *payload, usize payload_len, u32 tx_flags)
{
	struct bridge_dev *br = bridge_from_netdev(nd);
	if (!br)
		return -ENODEV;
	(void)tx_flags;

	if (!mac_is_group(hdr)) {
		struct netdev *out = bridge_lookup(br, hdr);
		if (out) {
			if (!netdev_is_admin_up(out))
				return -ENETDOWN;
			return netdev_transmit_frame(out, hdr, payload, payload_len, 0);
		}
	}
	int sent = 0;
	for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
		struct netdev *p = br->ports[i];
		if (!p || !netdev_is_admin_up(p))
			continue;
		if (netdev_transmit_frame(p, hdr, payload, payload_len, 0) == 0)
			sent = 1;
	}
	return sent ? 0 : -ENETDOWN;
}

/* Carrier: a bridge is up when at least one of its ports is. */
static int bridge_link_up(struct netdev *nd)
{
	struct bridge_dev *br = bridge_from_netdev(nd);
	if (!br)
		return 0;
	for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
		struct netdev *p = br->ports[i];
		if (!p || !netdev_is_admin_up(p))
			continue;
		if (!p->link_up || p->link_up(p) != 0)
			return 1;
	}
	return 0;
}

/* ── ports ──────────────────────────────────────────────────────────────── */

int bridge_add_port(struct netdev *brdev, struct netdev *port)
{
	struct bridge_dev *br = bridge_from_netdev(brdev);
	if (!br || !port)
		return -EINVAL;
	if (port == brdev || port->kind == NETDEV_KIND_BRIDGE)
		return -EINVAL;
	if (port->master)
		return -EBUSY;
	for (int i = 0; i < BRIDGE_MAX_PORTS; i++)
		if (br->ports[i] == port)
			return -EEXIST;
	for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
		if (br->ports[i])
			continue;
		br->ports[i] = port;
		port->master = brdev;
		return 0;
	}
	return -ENOSPC;
}

int bridge_del_port(struct netdev *brdev, struct netdev *port)
{
	struct bridge_dev *br = bridge_from_netdev(brdev);
	if (!br || !port)
		return -EINVAL;
	for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
		if (br->ports[i] != port)
			continue;
		br->ports[i] = 0;
		port->master = 0;
		bridge_forget_port(br, port);
		return 0;
	}
	return -ENOENT;
}

/* ── life cycle ─────────────────────────────────────────────────────────── */

static void bridge_destroy_dev(struct netdev *nd)
{
	struct bridge_dev *br = bridge_from_netdev(nd);
	if (!br)
		return;
	for (int i = 0; i < BRIDGE_MAX_PORTS; i++) {
		if (br->ports[i]) {
			br->ports[i]->master = 0;
			br->ports[i] = 0;
		}
	}
	netdev_unregister(nd);
	memset(br, 0, sizeof(*br));
}

int bridge_create(const char *name)
{
	if (!name || !name[0] || strlen(name) >= 16)
		return -EINVAL;
	if (netdev_index_by_name(name))
		return -EEXIST;

	struct bridge_dev *br = 0;
	int slot = 0;
	for (int i = 0; i < BRIDGE_MAX_DEVS; i++) {
		if (!bridges[i].used) {
			br = &bridges[i];
			slot = i;
			break;
		}
	}
	if (!br)
		return -ENOSPC;

	memset(br, 0, sizeof(*br));
	br->used = 1;
	br->nd.name = "bridge";
	strncpy(br->nd.ifname, name, sizeof(br->nd.ifname) - 1);
	/*
	 * A locally administered address of its own, rather than the first port's.
	 * Linux adopts a port's MAC; here a distinct address is what makes "this
	 * frame was addressed to the bridge" a different question from "this frame
	 * was addressed to the port", which is exactly the decision the forwarding
	 * path has to make.
	 */
	br->nd.mac.bytes[0] = 0x02;
	br->nd.mac.bytes[1] = 0xb1;
	br->nd.mac.bytes[2] = 0x60;
	br->nd.mac.bytes[3] = 0x00;
	br->nd.mac.bytes[4] = 0x00;
	br->nd.mac.bytes[5] = (u8)(0x10 + slot);
	br->nd.irq = -1;
	br->nd.kind = NETDEV_KIND_BRIDGE;
	br->nd.transmit = bridge_transmit;
	br->nd.link_up = bridge_link_up;
	br->nd.rx_from_port = bridge_rx_from_port;
	br->nd.destroy = bridge_destroy_dev;
	br->nd.priv = br;
	netdev_register(&br->nd);
	return 0;
}

usize bridge_snapshot(struct bridge_fdb_info *out, usize max)
{
	usize n = 0;
	u64 now = scheduler_get_uptime_ticks();
	for (int b = 0; b < BRIDGE_MAX_DEVS; b++) {
		struct bridge_dev *br = &bridges[b];
		if (!br->used)
			continue;
		for (int i = 0; i < BRIDGE_MAX_PORTS && n < max; i++) {
			if (!br->ports[i])
				continue;
			memset(&out[n], 0, sizeof(out[n]));
			strncpy(out[n].bridge, br->nd.ifname, sizeof(out[n].bridge) - 1);
			strncpy(out[n].port, br->ports[i]->ifname, sizeof(out[n].port) - 1);
			out[n].is_port_row = 1;
			n++;
		}
		for (int i = 0; i < BRIDGE_FDB_SIZE && n < max; i++) {
			struct bridge_fdb *e = &br->fdb[i];
			if (!e->used || now - e->stamp > BRIDGE_FDB_AGEING_TICKS)
				continue;
			memset(&out[n], 0, sizeof(out[n]));
			strncpy(out[n].bridge, br->nd.ifname, sizeof(out[n].bridge) - 1);
			if (e->port)
				strncpy(out[n].port, e->port->ifname, sizeof(out[n].port) - 1);
			memcpy(out[n].mac, e->mac, 6);
			out[n].age_ticks = (u32)(now - e->stamp);
			n++;
		}
	}
	return n;
}
