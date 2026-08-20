/* Active-backup bonding (M109).
 *
 * A bond is one interface standing in for several. In active-backup mode —
 * the only mode that means anything without a switch that agrees to
 * participate — exactly one slave carries traffic and the rest wait: the bond
 * transmits through the active slave, frames arriving on it are delivered as
 * if they had arrived on the bond, and when that slave goes down (carrier lost
 * or taken down by the operator) the next usable slave becomes active without
 * anything above the bond noticing.
 *
 * Frames arriving on a *backup* slave are dropped, as Linux does: two copies
 * of every broadcast is not redundancy, it is duplication.
 *
 * Created with `ip link add name bond0 type bond` and populated with
 * `ip link set eth1 master bond0`.
 */

#include <b1nix/vnet.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/errno.h>
#include <b1nix/console.h>
#include <string.h>

#define BOND_MAX_DEVS   2
#define BOND_MAX_SLAVES 4

struct bond_dev {
	struct netdev nd;
	struct netdev *slaves[BOND_MAX_SLAVES];
	struct netdev *active;
	u8 used;
};

static struct bond_dev bonds[BOND_MAX_DEVS];

static struct bond_dev *bond_from_netdev(struct netdev *nd)
{
	return nd ? (struct bond_dev *)nd->priv : 0;
}

/* A slave can carry traffic when the operator has not taken it down and its
 * driver does not report the carrier as lost (-1 = "cannot tell", which counts
 * as usable — refusing to use a device because it has no PHY to ask would rule
 * out virtio). */
static int bond_slave_usable(struct netdev *s)
{
	if (!s || !netdev_is_admin_up(s) || !s->transmit)
		return 0;
	if (s->link_up && s->link_up(s) == 0)
		return 0;
	return 1;
}

/* Keep the current slave if it is still usable, otherwise take the first one
 * that is. Returns the active slave, or NULL when none is left. */
static struct netdev *bond_select_active(struct bond_dev *b)
{
	if (bond_slave_usable(b->active))
		return b->active;
	struct netdev *prev = b->active;
	b->active = 0;
	for (int i = 0; i < BOND_MAX_SLAVES; i++) {
		if (!bond_slave_usable(b->slaves[i]))
			continue;
		b->active = b->slaves[i];
		break;
	}
	if (b->active != prev) {
		console_write("bond: ");
		console_write(b->nd.ifname);
		console_write(" active slave is now ");
		console_write(b->active ? b->active->ifname : "none");
		console_write("\n");
	}
	return b->active;
}

static int bond_transmit(struct netdev *nd, const u8 hdr[14],
                         const void *payload, usize payload_len, u32 tx_flags)
{
	struct bond_dev *b = bond_from_netdev(nd);
	if (!b)
		return -ENODEV;
	struct netdev *active = bond_select_active(b);
	if (!active)
		return -ENETDOWN;
	return netdev_transmit_frame(active, hdr, payload, payload_len, tx_flags);
}

static void bond_rx_from_port(struct netdev *master, struct netdev *port,
                              const void *frame, usize len, u32 rx_flags)
{
	struct bond_dev *b = bond_from_netdev(master);
	if (!b || !netdev_is_admin_up(master))
		return;
	if (bond_select_active(b) != port)
		return; /* a backup slave's copy of the same traffic */
	net_deliver_frame(master, frame, len, rx_flags);
}

static int bond_link_up(struct netdev *nd)
{
	struct bond_dev *b = bond_from_netdev(nd);
	if (!b)
		return 0;
	return bond_select_active(b) ? 1 : 0;
}

int bond_enslave(struct netdev *bonddev, struct netdev *slave)
{
	struct bond_dev *b = bond_from_netdev(bonddev);
	if (!b || !slave)
		return -EINVAL;
	if (slave == bonddev || slave->kind == NETDEV_KIND_BOND)
		return -EINVAL;
	if (slave->master)
		return -EBUSY;
	for (int i = 0; i < BOND_MAX_SLAVES; i++)
		if (b->slaves[i] == slave)
			return -EEXIST;
	for (int i = 0; i < BOND_MAX_SLAVES; i++) {
		if (b->slaves[i])
			continue;
		b->slaves[i] = slave;
		slave->master = bonddev;
		/* The bond takes the first slave's address, so failing over does not
		 * change the address its peers have in their ARP caches. */
		if (i == 0)
			b->nd.mac = slave->mac;
		bond_select_active(b);
		return 0;
	}
	return -ENOSPC;
}

int bond_release(struct netdev *bonddev, struct netdev *slave)
{
	struct bond_dev *b = bond_from_netdev(bonddev);
	if (!b || !slave)
		return -EINVAL;
	for (int i = 0; i < BOND_MAX_SLAVES; i++) {
		if (b->slaves[i] != slave)
			continue;
		b->slaves[i] = 0;
		slave->master = 0;
		if (b->active == slave)
			b->active = 0;
		bond_select_active(b);
		return 0;
	}
	return -ENOENT;
}

static void bond_destroy_dev(struct netdev *nd)
{
	struct bond_dev *b = bond_from_netdev(nd);
	if (!b)
		return;
	for (int i = 0; i < BOND_MAX_SLAVES; i++) {
		if (b->slaves[i]) {
			b->slaves[i]->master = 0;
			b->slaves[i] = 0;
		}
	}
	netdev_unregister(nd);
	memset(b, 0, sizeof(*b));
}

int bond_create(const char *name)
{
	if (!name || !name[0] || strlen(name) >= 16)
		return -EINVAL;
	if (netdev_index_by_name(name))
		return -EEXIST;

	struct bond_dev *b = 0;
	int slot = 0;
	for (int i = 0; i < BOND_MAX_DEVS; i++) {
		if (!bonds[i].used) {
			b = &bonds[i];
			slot = i;
			break;
		}
	}
	if (!b)
		return -ENOSPC;

	memset(b, 0, sizeof(*b));
	b->used = 1;
	b->nd.name = "bond";
	strncpy(b->nd.ifname, name, sizeof(b->nd.ifname) - 1);
	/* Until a slave arrives to lend its address, a locally administered one. */
	b->nd.mac.bytes[0] = 0x02;
	b->nd.mac.bytes[1] = 0xb1;
	b->nd.mac.bytes[2] = 0x60;
	b->nd.mac.bytes[5] = (u8)(0x20 + slot);
	b->nd.irq = -1;
	b->nd.kind = NETDEV_KIND_BOND;
	b->nd.transmit = bond_transmit;
	b->nd.link_up = bond_link_up;
	b->nd.rx_from_port = bond_rx_from_port;
	b->nd.destroy = bond_destroy_dev;
	b->nd.priv = b;
	netdev_register(&b->nd);
	return 0;
}

usize bond_snapshot(struct bond_info *out, usize max)
{
	usize n = 0;
	for (int i = 0; i < BOND_MAX_DEVS; i++) {
		struct bond_dev *b = &bonds[i];
		if (!b->used)
			continue;
		int any = 0;
		for (int s = 0; s < BOND_MAX_SLAVES && n < max; s++) {
			if (!b->slaves[s])
				continue;
			memset(&out[n], 0, sizeof(out[n]));
			strncpy(out[n].name, b->nd.ifname, sizeof(out[n].name) - 1);
			strncpy(out[n].slave, b->slaves[s]->ifname,
			        sizeof(out[n].slave) - 1);
			out[n].active = b->slaves[s] == b->active;
			n++;
			any = 1;
		}
		if (!any && n < max) {
			/* A bond with no slaves is still a bond, and saying so beats
			 * leaving it out of the listing entirely. */
			memset(&out[n], 0, sizeof(out[n]));
			strncpy(out[n].name, b->nd.ifname, sizeof(out[n].name) - 1);
			n++;
		}
	}
	return n;
}
