/* veth pairs (M109).
 *
 * A veth is a two-ended cable made of software: what one end transmits, the
 * other end receives, byte for byte, with nothing in between. That is the whole
 * device — no queue, no address, no forwarding decision.
 *
 * It exists because a network namespace is otherwise sealed. An interface
 * belongs to exactly one namespace, so a namespace with no interface of its own
 * has no way to reach anything; a veth pair is created in one namespace and one
 * of its ends is moved into another (`ip link set veth1 netns <pid>`), leaving a
 * single link that crosses the boundary. Everything `ip netns` does rests on
 * that one primitive.
 *
 * The peer receives through net_deliver_frame() rather than through its own
 * ->transmit: the frame has arrived as far as the peer is concerned, so it must
 * enter the receive path (packet sockets, bridge hand-off, protocol demux) and
 * not be sent a second time. net_deliver_frame() also pushes the receiving
 * device's namespace as the context, which is what makes a frame delivered into
 * another namespace resolve routes and neighbours in THAT namespace.
 *
 * Created with `ip link add veth0 type veth peer name veth1`; the rtnetlink
 * side is in kernel/net/netlink.c.
 */

#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/vnet.h>
#include <string.h>

#define VETH_MAX_DEVS 8

struct veth_dev {
	struct netdev nd;
	struct veth_dev *peer;
	u8 used;
};

static struct veth_dev veth_devs[VETH_MAX_DEVS];

static struct veth_dev *veth_from_netdev(struct netdev *nd)
{
	return nd ? (struct veth_dev *)nd->priv : 0;
}

/* A veth's carrier is up only while its peer exists and is administratively
 * up — exactly Linux's rule, and the reason `ip link add ... type veth` leaves
 * both ends NO-CARRIER until each is brought up. */
static int veth_link_up(struct netdev *nd)
{
	struct veth_dev *v = veth_from_netdev(nd);
	if (!v || !v->peer || !v->peer->used)
		return 0;
	return netdev_is_admin_up(&v->peer->nd) ? 1 : 0;
}

static int veth_transmit(struct netdev *nd, const u8 hdr[14],
                         const void *payload, usize payload_len, u32 tx_flags)
{
	struct veth_dev *v = veth_from_netdev(nd);

	(void)tx_flags; /* nothing between the ends can offload anything */
	if (!v || !v->peer || !v->peer->used)
		return -ENODEV;
	if (!netdev_is_admin_up(&v->peer->nd))
		return -ENETDOWN;
	if (payload_len > 65536)
		return -EMSGSIZE;

	/* The receive path takes one contiguous frame; the transmit interface
	 * hands over a header and a payload separately. One copy per frame is the
	 * cost of the cable. */
	usize total = 14 + payload_len;
	u8 *frame = kmalloc(total);
	if (!frame)
		return -ENOMEM;
	memcpy(frame, hdr, 14);
	if (payload_len)
		memcpy(frame + 14, payload, payload_len);
	net_deliver_frame(&v->peer->nd, frame, total, 0);
	kfree(frame);
	return 0;
}

/* `ip link del veth0` removes both ends, as Linux does: half a cable is not a
 * thing the device model can represent. */
static void veth_destroy_dev(struct netdev *nd)
{
	struct veth_dev *v = veth_from_netdev(nd);
	if (!v)
		return;
	struct veth_dev *peer = v->peer;
	if (peer) {
		peer->peer = 0;
		v->peer = 0;
	}
	netdev_unregister(nd);
	memset(v, 0, sizeof(*v));
	if (peer && peer->used) {
		netdev_unregister(&peer->nd);
		memset(peer, 0, sizeof(*peer));
	}
}

static struct veth_dev *veth_alloc(void)
{
	for (int i = 0; i < VETH_MAX_DEVS; i++) {
		if (!veth_devs[i].used)
			return &veth_devs[i];
	}
	return 0;
}

static void veth_init_end(struct veth_dev *v, const char *name, u8 slot)
{
	memset(v, 0, sizeof(*v));
	v->used = 1;
	v->nd.name = "veth";
	strncpy(v->nd.ifname, name, sizeof(v->nd.ifname) - 1);
	v->nd.irq = -1;
	v->nd.kind = NETDEV_KIND_VETH;
	v->nd.transmit = veth_transmit;
	v->nd.link_up = veth_link_up;
	v->nd.destroy = veth_destroy_dev;
	v->nd.priv = v;
	/* Locally administered address, distinct per end, in the same block the
	 * other virtual devices use. */
	v->nd.mac.bytes[0] = 0x02;
	v->nd.mac.bytes[1] = 0xb1;
	v->nd.mac.bytes[2] = 0x60;
	v->nd.mac.bytes[3] = 0x00;
	v->nd.mac.bytes[4] = 0x0e;
	v->nd.mac.bytes[5] = slot;
}

int veth_create(const char *name, const char *peer_name)
{
	if (!name || !name[0] || strlen(name) >= 16)
		return -EINVAL;
	if (!peer_name || !peer_name[0] || strlen(peer_name) >= 16)
		return -EINVAL;
	if (strcmp(name, peer_name) == 0)
		return -EINVAL;
	if (netdev_index_by_name(name) || netdev_index_by_name(peer_name))
		return -EEXIST;

	struct veth_dev *a = veth_alloc();
	if (!a)
		return -ENOSPC;
	a->used = 1; /* claim it so the second allocation cannot pick it too */
	struct veth_dev *b = veth_alloc();
	if (!b) {
		a->used = 0;
		return -ENOSPC;
	}

	veth_init_end(a, name, (u8)(a - veth_devs));
	veth_init_end(b, peer_name, (u8)(b - veth_devs));
	a->peer = b;
	b->peer = a;

	/* Both ends are born in the creator's namespace; moving one across is a
	 * separate, explicit step. */
	netdev_register(&a->nd);
	netdev_register(&b->nd);
	return 0;
}
