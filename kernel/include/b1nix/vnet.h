#ifndef B1NIX_VNET_H
#define B1NIX_VNET_H

#include <b1nix/types.h>
#include <b1nix/net.h>

/*
 * Virtual network devices (M109).
 *
 * Four devices the stack builds itself and stacks on top of a real NIC, each
 * one a struct netdev with a transmit function that hands frames to a lower
 * device:
 *
 *   vlan    802.1Q — tags on transmit, matches the VID and strips the tag on
 *           receive, and carries its own MAC and interface index.
 *   bridge  a software bridge: a forwarding database learned from source
 *           addresses, flooding for destinations it has not learned yet, and
 *           ports added and removed while it runs.
 *   bond    active-backup aggregation: transmits through whichever slave is
 *           up, and moves to the next one when that stops being true.
 *   gretap  ethernet over GRE over IPv4 (RFC 2784 + the transparent-bridging
 *           protocol type), which is what gives a virtual device a wire.
 *
 * They are created and destroyed through rtnetlink — RTM_NEWLINK with
 * IFLA_LINKINFO, RTM_DELLINK, and IFLA_MASTER to enslave a port — which is
 * exactly what `ip link add ... type bridge` and `ip link set eth1 master br0`
 * send.
 */

struct netdev;

/* ── creation / destruction, called from the rtnetlink layer ────────────── */

/* All four return 0, or a negative errno (-EEXIST for a name already taken,
 * -ENOSPC when the fixed device table is full, -EINVAL for bad arguments). */
int vlan_create(const char *name, struct netdev *lower, u16 vid);
int bridge_create(const char *name);
int bond_create(const char *name);
int gretap_create(const char *name, struct ipv4_addr local,
                  struct ipv4_addr remote, u32 ikey, u32 okey, int has_key);
/* Both ends at once — a veth is a pair or it is nothing. Deleting either end
 * removes both. */
int veth_create(const char *name, const char *peer_name);

/* Destroy a device created here. -EOPNOTSUPP for a driver's NIC. */
int vnet_destroy(struct netdev *nd);

/* Port membership, used by vnet_set_master(). */
int bridge_add_port(struct netdev *br, struct netdev *port);
int bridge_del_port(struct netdev *br, struct netdev *port);
int bond_enslave(struct netdev *bond, struct netdev *slave);
int bond_release(struct netdev *bond, struct netdev *slave);

/* `ip link set <dev> master <br>` / `nomaster`. Enslaving refuses a device
 * that is already enslaved, a master enslaved to itself, and a device that is
 * carrying the L3 configuration — taking the address out from under the stack
 * is not something a link command should do silently. */
int vnet_set_master(struct netdev *dev, struct netdev *master);

/* ── receive hooks, called from ethernet_receive_flags() ────────────────── */

/* An 802.1Q frame arrived on `lower`. Returns 1 when a VLAN device claimed it
 * (and has already been handed the untagged frame), 0 when no device on this
 * interface is configured for that VID. */
int vlan_rx(struct netdev *lower, const void *frame, usize len, u32 rx_flags);

/* ── GRE, called from the IPv4 demux ────────────────────────────────────── */

/* IPPROTO_GRE payload of a datagram from `src` addressed to us. */
void gre_input(struct ipv4_addr src, const void *data, usize len);

/* ── /proc/net views ────────────────────────────────────────────────────── */

/* Flat snapshots, formatted by procfs. Each returns the number of entries
 * written, which is never more than `max`. */

struct vlan_info {
	char name[16];
	char lower[16];
	u16 vid;
};
usize vlan_snapshot(struct vlan_info *out, usize max);

/* One row per learned address, plus one row per port with an empty MAC so a
 * bridge with no traffic on it still shows its ports. */
struct bridge_fdb_info {
	char bridge[16];
	char port[16];
	u8 mac[6];
	u8 is_port_row;
	u32 age_ticks;
};
usize bridge_snapshot(struct bridge_fdb_info *out, usize max);

/* One row per slave; `active` marks the one currently carrying traffic. */
struct bond_info {
	char name[16];
	char slave[16];
	u8 active;
};
usize bond_snapshot(struct bond_info *out, usize max);

#endif
