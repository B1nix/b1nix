#ifndef B1NIX_U_LINUX_IF_VLAN_H
#define B1NIX_U_LINUX_IF_VLAN_H

#include <linux/types.h>

/* VLAN flags / nested attribute ids, enough for BusyBox `ip link` to compile
 * (b1nix does not configure VLANs). */
#define VLAN_FLAG_REORDER_HDR   0x1
#define VLAN_FLAG_GVRP          0x2
#define VLAN_FLAG_LOOSE_BINDING 0x4
#define VLAN_FLAG_MVRP          0x8

struct ifla_vlan_flags {
  __u32 flags;
  __u32 mask;
};

enum {
  IFLA_VLAN_UNSPEC,
  IFLA_VLAN_ID,
  IFLA_VLAN_FLAGS,
  IFLA_VLAN_EGRESS_QOS,
  IFLA_VLAN_INGRESS_QOS,
  IFLA_VLAN_PROTOCOL,
  __IFLA_VLAN_MAX
};
#define IFLA_VLAN_MAX (__IFLA_VLAN_MAX - 1)

#endif
