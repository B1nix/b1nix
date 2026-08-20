/* GRE tunnel devices — gretap (M109).
 *
 * The tunnel this stack can genuinely carry is the ethernet one: a gretap
 * device is a struct netdev whose "wire" is an IPv4 datagram. A frame it
 * transmits is wrapped in a GRE header (RFC 2784, protocol type 0x6558,
 * transparent ethernet bridging) and sent to the remote endpoint as IP
 * protocol 47; a GRE datagram arriving from that endpoint is unwrapped and the
 * frame inside is delivered as if it had arrived on the tunnel device.
 *
 * The IPIP/sit tunnels Linux also offers carry a bare IP packet with no
 * ethernet header, which does not fit the netdev model this kernel is built
 * around (every device transmits a frame, not a datagram) — that is why
 * gretap, and not ipip, is what exists here.
 *
 * Created with:
 *   ip link add name gre0 type gretap local 10.0.2.15 remote 10.0.2.2 [key K]
 */

#include <b1nix/vnet.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <string.h>

#define GRE_MAX_DEVS 4
#define GRE_PROTO_TEB 0x6558   /* transparent ethernet bridging */
#define GRE_FLAG_KEY  0x2000   /* the key field is present */

struct gre_dev {
	struct netdev nd;
	struct ipv4_addr local;
	struct ipv4_addr remote;
	u32 ikey;
	u32 okey;
	u8 has_key;
	u8 used;
};

static struct gre_dev gre_devs[GRE_MAX_DEVS];

static struct gre_dev *gre_from_netdev(struct netdev *nd)
{
	return nd ? (struct gre_dev *)nd->priv : 0;
}

static void put_be16(u8 *p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)(v & 0xFF);
}

static void put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)((v >> 16) & 0xFF);
	p[2] = (u8)((v >> 8) & 0xFF);
	p[3] = (u8)(v & 0xFF);
}

static u16 load_be16(const u8 *p)
{
	return (u16)((p[0] << 8) | p[1]);
}

static u32 load_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static int gre_transmit(struct netdev *nd, const u8 hdr[14],
                        const void *payload, usize payload_len, u32 tx_flags)
{
	(void)tx_flags;
	struct gre_dev *t = gre_from_netdev(nd);
	if (!t)
		return -ENODEV;

	usize ghdr = t->has_key ? 8u : 4u;
	usize total = ghdr + 14 + payload_len;
	u8 *buf = kmalloc(total);
	if (!buf)
		return -ENOMEM;
	put_be16(buf, t->has_key ? GRE_FLAG_KEY : 0);
	put_be16(buf + 2, GRE_PROTO_TEB);
	if (t->has_key)
		put_be32(buf + 4, t->okey);
	memcpy(buf + ghdr, hdr, 14);
	memcpy(buf + ghdr + 14, payload, payload_len);

	/* The IP layer picks the route, the source address and the interface —
	 * the tunnel only says where the far end is. A remote of 127.0.0.1 rides
	 * the loopback datapath, which is a real encapsulate/decapsulate round
	 * trip through the same code an off-box endpoint would take. */
	ipv4_send(t->remote, 47 /* IPPROTO_GRE */, buf, total);
	kfree(buf);
	return 0;
}

/* A tunnel has no PHY. It is up when it has an endpoint configured. */
static int gre_link_up(struct netdev *nd)
{
	struct gre_dev *t = gre_from_netdev(nd);
	if (!t)
		return 0;
	for (int i = 0; i < 4; i++)
		if (t->remote.bytes[i])
			return 1;
	return 0;
}

void gre_input(struct ipv4_addr src, const void *data, usize len)
{
	if (!data || len < 4)
		return;
	const u8 *p = (const u8 *)data;
	u16 flags = load_be16(p);
	u16 proto = load_be16(p + 2);
	/* RFC 2784 defines version 0 with the reserved bits clear; anything else
	 * (checksums, sequence numbers, routing) is not something this
	 * implementation can parse, and guessing at the header length would hand
	 * the wrong bytes to the receive path. */
	if ((flags & 0x0007) != 0)
		return;
	if ((flags & ~(u16)GRE_FLAG_KEY) != 0)
		return;
	if (proto != GRE_PROTO_TEB)
		return;

	usize ghdr = 4;
	u32 key = 0;
	if (flags & GRE_FLAG_KEY) {
		if (len < 8)
			return;
		key = load_be32(p + 4);
		ghdr = 8;
	}
	if (len < ghdr + 14)
		return;

	for (int i = 0; i < GRE_MAX_DEVS; i++) {
		struct gre_dev *t = &gre_devs[i];
		if (!t->used || !netdev_is_admin_up(&t->nd))
			continue;
		if (memcmp(t->remote.bytes, src.bytes, 4) != 0)
			continue;
		if (t->has_key != ((flags & GRE_FLAG_KEY) ? 1 : 0))
			continue;
		if (t->has_key && t->ikey != key)
			continue;
		/* The datagram was reassembled and checksummed by the IP layer; the
		 * frame inside never crossed a wire of its own. */
		net_deliver_frame(&t->nd, p + ghdr, len - ghdr, NET_RX_F_CSUM_OK);
		return;
	}
}

static void gre_destroy_dev(struct netdev *nd)
{
	struct gre_dev *t = gre_from_netdev(nd);
	if (!t)
		return;
	netdev_unregister(nd);
	memset(t, 0, sizeof(*t));
}

int gretap_create(const char *name, struct ipv4_addr local,
                  struct ipv4_addr remote, u32 ikey, u32 okey, int has_key)
{
	if (!name || !name[0] || strlen(name) >= 16)
		return -EINVAL;
	if (netdev_index_by_name(name))
		return -EEXIST;
	int have_remote = 0;
	for (int i = 0; i < 4; i++)
		if (remote.bytes[i])
			have_remote = 1;
	if (!have_remote)
		return -EINVAL; /* a tunnel with no far end carries nothing */

	struct gre_dev *t = 0;
	int slot = 0;
	for (int i = 0; i < GRE_MAX_DEVS; i++) {
		if (!gre_devs[i].used) {
			t = &gre_devs[i];
			slot = i;
			break;
		}
	}
	if (!t)
		return -ENOSPC;

	memset(t, 0, sizeof(*t));
	t->used = 1;
	t->local = local;
	t->remote = remote;
	t->ikey = ikey;
	t->okey = okey;
	t->has_key = has_key ? 1 : 0;
	t->nd.name = "gretap";
	strncpy(t->nd.ifname, name, sizeof(t->nd.ifname) - 1);
	/* Linux gives a gretap a random address; a locally administered one keyed
	 * on the slot is the same thing without a source of randomness. */
	t->nd.mac.bytes[0] = 0x02;
	t->nd.mac.bytes[1] = 0xb1;
	t->nd.mac.bytes[2] = 0x60;
	t->nd.mac.bytes[5] = (u8)(0x30 + slot);
	t->nd.irq = -1;
	t->nd.kind = NETDEV_KIND_GRETAP;
	t->nd.transmit = gre_transmit;
	t->nd.link_up = gre_link_up;
	t->nd.destroy = gre_destroy_dev;
	t->nd.priv = t;
	netdev_register(&t->nd);
	return 0;
}
