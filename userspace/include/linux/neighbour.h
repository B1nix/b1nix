#ifndef B1NIX_U_LINUX_NEIGHBOUR_H
#define B1NIX_U_LINUX_NEIGHBOUR_H

#include <linux/types.h>
#include <linux/netlink.h>

/* Neighbour-table netlink defs. b1nix implements RTM_GETNEIGH/NEWNEIGH/DELNEIGH
 * for AF_INET (the ARP cache) and, with ndp.ko loaded, for AF_INET6. */
struct ndmsg {
  __u8 ndm_family;
  __u8 ndm_pad1;
  __u16 ndm_pad2;
  __s32 ndm_ifindex;
  __u16 ndm_state;
  __u8 ndm_flags;
  __u8 ndm_type;
};

enum {
  NDA_UNSPEC,
  NDA_DST,
  NDA_LLADDR,
  NDA_CACHEINFO,
  NDA_PROBES,
  __NDA_MAX
};
#define NDA_MAX (__NDA_MAX - 1)

/* NDA_CACHEINFO's payload. b1nix reports no ageing timers, so the fields are
 * zero, but `ip neigh` still dereferences the struct when the attribute is
 * present in someone else's dump. */
struct nda_cacheinfo {
  __u32 ndm_confirmed;
  __u32 ndm_used;
  __u32 ndm_updated;
  __u32 ndm_refcnt;
};

#define NDA_RTA(r)                                                             \
  ((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))
#define NDA_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct ndmsg))

/* ndm_flags */
#define NTF_PROXY  0x08
#define NTF_ROUTER 0x80

/* ndm_state */
#define NUD_INCOMPLETE 0x01
#define NUD_REACHABLE  0x02
#define NUD_STALE      0x04
#define NUD_DELAY      0x08
#define NUD_PROBE      0x10
#define NUD_FAILED     0x20
#define NUD_NOARP      0x40
#define NUD_PERMANENT  0x80
#define NUD_NONE       0x00

#endif
