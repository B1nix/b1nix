/* 802.1Q VLAN devices (M109).
 *
 * A vlan device is a struct netdev stacked on a real one: everything it
 * transmits leaves the lower interface with a 4-byte 802.1Q tag inserted after
 * the source MAC, and every tagged frame arriving on that lower interface with
 * a matching VID is stripped and delivered as if it had arrived on the vlan
 * device itself. Its MAC is the lower device's (which is what Linux does), and
 * it holds an interface index of its own, so AF_PACKET, `ip link` and the FIB
 * all see it as an interface rather than as a property of another one.
 *
 * Created with `ip link add link eth0 name vlan10 type vlan id 10`; see
 * kernel/net/netlink.c for the rtnetlink side.
 */

#include <b1nix/vnet.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <string.h>

#define VLAN_MAX_DEVS 8
#define ETHERTYPE_VLAN 0x8100
/* Tag control information: 3 bits priority, 1 drop-eligible, 12 bits VID. */
#define VLAN_VID_MASK 0x0FFF

struct vlan_dev {
	struct netdev nd;
	u16 vid;
	u8 used;
};

static struct vlan_dev vlan_devs[VLAN_MAX_DEVS];

static struct vlan_dev *vlan_from_netdev(struct netdev *nd)
{
	return nd ? (struct vlan_dev *)nd->priv : 0;
}

/*
 * Transmit: the caller's 14-byte header keeps its addresses, its ethertype
 * moves inside the tag, and 0x8100 takes its place. The tag and the payload
 * have to be contiguous for the lower driver's (header, payload) interface, so
 * a copy is unavoidable here — it is four bytes plus the frame, once per
 * packet, which is what tagging costs.
 */
static int vlan_transmit(struct netdev *nd, const u8 hdr[14],
                         const void *payload, usize payload_len, u32 tx_flags)
{
	struct vlan_dev *v = vlan_from_netdev(nd);
	struct netdev *lower = nd->lower;
	if (!v || !lower)
		return -ENODEV;
	if (!netdev_is_admin_up(lower))
		return -ENETDOWN;

	u8 lower_hdr[14];
	memcpy(lower_hdr, hdr, 12);
	lower_hdr[12] = (ETHERTYPE_VLAN >> 8) & 0xFF;
	lower_hdr[13] = ETHERTYPE_VLAN & 0xFF;

	usize tagged_len = payload_len + 4;
	u8 *buf = kmalloc(tagged_len);
	if (!buf)
		return -ENOMEM;
	buf[0] = (u8)((v->vid >> 8) & 0x0F);  /* priority 0, DEI 0 */
	buf[1] = (u8)(v->vid & 0xFF);
	buf[2] = hdr[12];
	buf[3] = hdr[13];
	memcpy(buf + 4, payload, payload_len);

	int rc = netdev_transmit_frame(lower, lower_hdr, buf, tagged_len, tx_flags);
	kfree(buf);
	return rc;
}

/* Carrier follows the lower device: a VLAN over a dead link is dead. */
static int vlan_link_up(struct netdev *nd)
{
	struct netdev *lower = nd->lower;
	if (!lower)
		return 0;
	return lower->link_up ? lower->link_up(lower) : -1;
}

int vlan_rx(struct netdev *lower, const void *frame, usize len, u32 rx_flags)
{
	/* 14 header + 4 tag is the shortest thing that can carry a VID. */
	if (!lower || len < 18)
		return 0;
	const u8 *f = (const u8 *)frame;
	u16 vid = (u16)(((f[14] & 0x0F) << 8) | f[15]) & VLAN_VID_MASK;

	struct vlan_dev *match = 0;
	for (int i = 0; i < VLAN_MAX_DEVS; i++) {
		if (vlan_devs[i].used && vlan_devs[i].nd.lower == lower &&
		    vlan_devs[i].vid == vid) {
			match = &vlan_devs[i];
			break;
		}
	}
	/* No device for this VID: the frame is not ours to strip. Reporting that
	 * honestly lets the caller drop it rather than feed a tagged frame to a
	 * protocol demux that would misread the tag as an ethertype. */
	if (!match || !netdev_is_admin_up(&match->nd))
		return 0;

	usize inner_len = len - 4;
	u8 *buf = kmalloc(inner_len);
	if (!buf)
		return 1; /* claimed, but dropped: an untagged copy could not be made */
	memcpy(buf, f, 12);
	memcpy(buf + 12, f + 16, len - 16);
	net_deliver_frame(&match->nd, buf, inner_len, rx_flags);
	kfree(buf);
	return 1;
}

/* `ip link del vlan10`: reached through vnet_destroy() -> nd->destroy. */
static void vlan_destroy_dev(struct netdev *nd)
{
	struct vlan_dev *v = vlan_from_netdev(nd);
	if (!v)
		return;
	netdev_unregister(nd);
	memset(v, 0, sizeof(*v));
}

int vlan_create(const char *name, struct netdev *lower, u16 vid)
{
	if (!name || !name[0] || strlen(name) >= 16 || !lower)
		return -EINVAL;
	/* 0 is "priority tagged, no VLAN" and 4095 is reserved. */
	if (vid == 0 || vid >= 4095)
		return -EINVAL;
	if (netdev_index_by_name(name))
		return -EEXIST;
	for (int i = 0; i < VLAN_MAX_DEVS; i++) {
		if (vlan_devs[i].used && vlan_devs[i].nd.lower == lower &&
		    vlan_devs[i].vid == vid)
			return -EEXIST;
	}

	struct vlan_dev *v = 0;
	for (int i = 0; i < VLAN_MAX_DEVS; i++) {
		if (!vlan_devs[i].used) {
			v = &vlan_devs[i];
			break;
		}
	}
	if (!v)
		return -ENOSPC;

	memset(v, 0, sizeof(*v));
	v->used = 1;
	v->vid = vid;
	v->nd.name = "vlan";
	strncpy(v->nd.ifname, name, sizeof(v->nd.ifname) - 1);
	v->nd.mac = lower->mac;
	v->nd.irq = -1;
	v->nd.kind = NETDEV_KIND_VLAN;
	v->nd.lower = lower;
	v->nd.transmit = vlan_transmit;
	v->nd.link_up = vlan_link_up;
	v->nd.destroy = vlan_destroy_dev;
	v->nd.priv = v;
	netdev_register(&v->nd);
	return 0;
}

usize vlan_snapshot(struct vlan_info *out, usize max)
{
	usize n = 0;
	for (int i = 0; i < VLAN_MAX_DEVS && n < max; i++) {
		if (!vlan_devs[i].used)
			continue;
		memset(&out[n], 0, sizeof(out[n]));
		strncpy(out[n].name, vlan_devs[i].nd.ifname, sizeof(out[n].name) - 1);
		struct netdev *lower = vlan_devs[i].nd.lower;
		if (lower)
			strncpy(out[n].lower, lower->ifname, sizeof(out[n].lower) - 1);
		out[n].vid = vlan_devs[i].vid;
		n++;
	}
	return n;
}
