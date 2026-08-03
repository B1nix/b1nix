/* AF_NETLINK / NETLINK_ROUTE (rtnetlink) — M107.
 *
 * BusyBox `ip`, and every modern replacement for ifconfig/route/arp, speaks
 * rtnetlink and nothing else. This file implements the protocol against the
 * tables b1nix really has:
 *
 *   RTM_GETLINK   -> the netdev registry (kernel/net/net.c) plus a synthetic
 *                    loopback interface; MAC, MTU and the carrier state the
 *                    driver's ->link_up reports.
 *   RTM_GETADDR   -> the interface address state (net_get_ip/netmask, the
 *                    IPv6 link-local and SLAAC global), 127.0.0.1/8 and ::1
 *                    on the loopback index.
 *   RTM_GETROUTE  -> route_snapshot()/route6_snapshot(), the same FIB the
 *                    datapath looks up in.
 *   RTM_GETNEIGH  -> arp_snapshot() and, through the protocol registry,
 *                    ndp.ko's neighbour cache — the real caches.
 *   RTM_NEW/DELROUTE, RTM_NEW/DELNEIGH, RTM_NEW/DELADDR mutate that same
 *                    state through the ordinary kernel APIs.
 *
 * A request is serviced synchronously: the reply is encoded and pushed into
 * the socket's own datagram queue, so the recvmsg() that follows returns it.
 * Requests we cannot honestly satisfy (creating or deleting an interface, or
 * changing a link property other than IFF_UP) are answered with a real
 * NLMSG_ERROR carrying -EOPNOTSUPP rather than a silent success.
 */

#include <b1nix/netlink.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/netproto.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>
#include <string.h>

/* ── Wire constants (Linux ABI) ─────────────────────────────────────────── */

#define NLMSG_NOOP   1
#define NLMSG_ERROR  2
#define NLMSG_DONE   3

#define NLM_F_REQUEST 0x001
#define NLM_F_MULTI   0x002
#define NLM_F_ACK     0x004
#define NLM_F_ROOT    0x100
#define NLM_F_MATCH   0x200
#define NLM_F_DUMP    (NLM_F_ROOT | NLM_F_MATCH)
#define NLM_F_CREATE  0x400

#define RTM_NEWLINK  16
#define RTM_DELLINK  17
#define RTM_GETLINK  18
#define RTM_SETLINK  19
#define RTM_NEWADDR  20
#define RTM_DELADDR  21
#define RTM_GETADDR  22
#define RTM_NEWROUTE 24
#define RTM_DELROUTE 25
#define RTM_GETROUTE 26
#define RTM_NEWNEIGH 28
#define RTM_DELNEIGH 29
#define RTM_GETNEIGH 30

/* rtattr types */
#define IFLA_ADDRESS   1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME    3
#define IFLA_MTU       4
#define IFLA_QDISC     6
#define IFLA_STATS     7
#define IFLA_TXQLEN    13
#define IFLA_OPERSTATE 16
#define IFLA_LINKMODE  17

#define IFA_ADDRESS   1
#define IFA_LOCAL     2
#define IFA_LABEL     3
#define IFA_BROADCAST 4

#define RTA_DST     1
#define RTA_OIF     4
#define RTA_GATEWAY 5
#define RTA_PRIORITY 6
#define RTA_PREFSRC 7
#define RTA_TABLE   15

#define NDA_DST    1
#define NDA_LLADDR 2

/* interface flags */
#define NL_IFF_UP        0x0001
#define NL_IFF_BROADCAST 0x0002
#define NL_IFF_LOOPBACK  0x0008
#define NL_IFF_RUNNING   0x0040
#define NL_IFF_MULTICAST 0x1000

#define ARPHRD_ETHER    1
#define ARPHRD_LOOPBACK 772

/* route attributes */
#define RTN_UNICAST 1
#define RTPROT_BOOT   3
#define RTPROT_STATIC 4
#define RTPROT_DHCP   16
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK     253
#define RT_SCOPE_HOST     254

#define NUD_REACHABLE 0x02
#define NUD_PERMANENT 0x80

#define IF_OPER_UP   6
#define IF_OPER_DOWN 2

/* A reply carries the *requester's* port id in nlmsg_pid, not the kernel's:
 * libnetlink's dump filter drops any message whose nlmsg_pid does not match
 * the port id it read back from getsockname(), so answering with 0 would make
 * `ip` show an empty table. The socket's bound address holds that id. */
static u32 nl_sock_pid(const struct vfs_socket_state *s) {
  struct b1nix_sockaddr_nl nl;
  memcpy(&nl, &s->local, sizeof(nl));
  return nl.nl_family == B1NIX_AF_NETLINK ? nl.nl_pid : 0;
}

/* One dump is assembled here before being chopped into datagrams. Sized for
 * the worst case this kernel can produce: 8 interfaces + their addresses, 64
 * IPv4 and 64 IPv6 routes, 64 ARP and 16 NDP neighbours. */
#define NL_DUMP_MAX 16384
/* A socket queue slot holds 2048 bytes (struct vfs_socket_state), and the
 * queue is 8 deep. */
#define NL_SLOT_MAX 2048

/* ── Little-endian encoders (x86_64 is LE; be explicit anyway) ───────────── */

static usize nl_align(usize n) { return (n + 3u) & ~(usize)3u; }

struct nl_buf {
  u8 *p;
  usize cap;
  usize len;
  int overflow;
};

static void nl_put_u8(struct nl_buf *b, u8 v) {
  if (b->len + 1 > b->cap) {
    b->overflow = 1;
    return;
  }
  b->p[b->len++] = v;
}

static void nl_put_bytes(struct nl_buf *b, const void *src, usize n) {
  if (b->len + n > b->cap) {
    b->overflow = 1;
    return;
  }
  if (src)
    memcpy(b->p + b->len, src, n);
  else
    memset(b->p + b->len, 0, n);
  b->len += n;
}

static void nl_pad(struct nl_buf *b, usize to) {
  while (b->len < to && !b->overflow)
    nl_put_u8(b, 0);
}

static void nl_store_u16(u8 *p, u16 v) {
  p[0] = (u8)v;
  p[1] = (u8)(v >> 8);
}

static void nl_store_u32(u8 *p, u32 v) {
  p[0] = (u8)v;
  p[1] = (u8)(v >> 8);
  p[2] = (u8)(v >> 16);
  p[3] = (u8)(v >> 24);
}

static u16 nl_load_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static u32 nl_load_u32(const u8 *p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Start an nlmsghdr; returns its offset so nl_msg_end can back-fill the
 * length. */
static usize nl_msg_begin(struct nl_buf *b, u16 type, u16 flags, u32 seq,
                          u32 pid) {
  usize off = b->len;
  u8 hdr[16];
  memset(hdr, 0, sizeof(hdr));
  nl_store_u16(hdr + 4, type);
  nl_store_u16(hdr + 6, flags);
  nl_store_u32(hdr + 8, seq);
  nl_store_u32(hdr + 12, pid);
  nl_put_bytes(b, hdr, sizeof(hdr));
  return off;
}

static void nl_msg_end(struct nl_buf *b, usize off) {
  if (b->overflow || off + 16 > b->len)
    return;
  nl_store_u32(b->p + off, (u32)(b->len - off));
  nl_pad(b, nl_align(b->len));
}

static void nl_put_attr(struct nl_buf *b, u16 type, const void *data,
                        u16 dlen) {
  u8 hdr[4];
  nl_store_u16(hdr + 0, (u16)(4 + dlen));
  nl_store_u16(hdr + 2, type);
  nl_put_bytes(b, hdr, 4);
  nl_put_bytes(b, data, dlen);
  nl_pad(b, nl_align(b->len));
}

static void nl_put_attr_u32(struct nl_buf *b, u16 type, u32 v) {
  u8 tmp[4];
  nl_store_u32(tmp, v);
  nl_put_attr(b, type, tmp, 4);
}

/* ── The interface table netlink presents ───────────────────────────────── */

struct nl_iface {
  int index;
  char name[8];
  int loopback;
  u16 arptype;
  u32 mtu;
  u32 flags;
  struct mac_addr mac;
};

/* Fill `out` with every interface. Real NICs occupy the netdev registry's own
 * 1-based indices so a route's oif and /proc/net/route agree with what `ip`
 * prints; loopback gets NETLINK_LO_IFINDEX because it is not a netdev. */
static usize nl_iface_table(struct nl_iface *out, usize max) {
  usize n = 0;
  for (int idx = 1; n < max; idx++) {
    struct netdev *nd = netdev_by_index(idx);
    if (!nd)
      break;
    memset(&out[n], 0, sizeof(out[n]));
    out[n].index = idx;
    netdev_ifname(idx, out[n].name, sizeof(out[n].name));
    out[n].arptype = ARPHRD_ETHER;
    out[n].mtu = 1500;
    out[n].flags = NL_IFF_BROADCAST | NL_IFF_MULTICAST;
    /* IFF_UP is the administrative state the operator set; RUNNING means
     * carrier, and that is a real question the driver answers. link_up() == -1
     * (no carrier indication, e.g. virtio) counts as up. A down interface has
     * neither flag — `ip link` must show what `ip link set down` did. */
    if (netdev_is_admin_up(nd)) {
      out[n].flags |= NL_IFF_UP;
      if (!nd->link_up || nd->link_up(nd) != 0)
        out[n].flags |= NL_IFF_RUNNING;
    }
    out[n].mac = netdev_index_of(nd) == netdev_index_of(netdev_active())
                     ? net_get_mac()
                     : nd->mac;
    n++;
  }
  if (n < max) {
    memset(&out[n], 0, sizeof(out[n]));
    out[n].index = NETLINK_LO_IFINDEX;
    out[n].name[0] = 'l';
    out[n].name[1] = 'o';
    out[n].name[2] = '\0';
    out[n].loopback = 1;
    out[n].arptype = ARPHRD_LOOPBACK;
    out[n].mtu = 65536;
    out[n].flags = NL_IFF_UP | NL_IFF_LOOPBACK | NL_IFF_RUNNING;
    n++;
  }
  return n;
}

#define NL_MAX_IFACES 12

static int nl_iface_by_index(int index, struct nl_iface *out) {
  struct nl_iface tab[NL_MAX_IFACES];
  usize n = nl_iface_table(tab, NL_MAX_IFACES);
  for (usize i = 0; i < n; i++)
    if (tab[i].index == index) {
      *out = tab[i];
      return 1;
    }
  return 0;
}

/* Netmask -> prefix length. Only contiguous masks exist in the FIB. */
static u8 nl_mask_to_plen(u32 mask) {
  u8 bits = 0;
  for (int i = 31; i >= 0; i--) {
    if (!(mask & (1u << i)))
      break;
    bits++;
  }
  return bits;
}

static u32 nl_plen_to_mask(u8 plen) {
  if (plen == 0)
    return 0;
  if (plen >= 32)
    return 0xFFFFFFFFu;
  return (u32)(0xFFFFFFFFu << (32 - plen));
}

static int nl_in6_is_zero(const u8 *a) {
  for (int i = 0; i < 16; i++)
    if (a[i])
      return 0;
  return 1;
}

/* ── Dump builders ──────────────────────────────────────────────────────── */

static void nl_dump_links(struct nl_buf *b, u32 seq, u32 pid, u8 family) {
  struct nl_iface tab[NL_MAX_IFACES];
  usize n = nl_iface_table(tab, NL_MAX_IFACES);
  for (usize i = 0; i < n; i++) {
    usize off = nl_msg_begin(b, RTM_NEWLINK, NLM_F_MULTI, seq, pid);
    u8 ifi[16];
    memset(ifi, 0, sizeof(ifi));
    ifi[0] = family ? family : 0; /* AF_UNSPEC */
    nl_store_u16(ifi + 2, tab[i].arptype);
    nl_store_u32(ifi + 4, (u32)tab[i].index);
    nl_store_u32(ifi + 8, tab[i].flags);
    nl_put_bytes(b, ifi, sizeof(ifi));
    nl_put_attr(b, IFLA_IFNAME, tab[i].name, (u16)(strlen(tab[i].name) + 1));
    nl_put_attr_u32(b, IFLA_MTU, tab[i].mtu);
    nl_put_attr_u32(b, IFLA_TXQLEN, tab[i].loopback ? 0 : 1000);
    u8 oper = (tab[i].flags & NL_IFF_RUNNING) ? IF_OPER_UP : IF_OPER_DOWN;
    nl_put_attr(b, IFLA_OPERSTATE, &oper, 1);
    u8 linkmode = 0;
    nl_put_attr(b, IFLA_LINKMODE, &linkmode, 1);
    nl_put_attr(b, IFLA_QDISC, "noqueue", 8);
    if (tab[i].loopback) {
      u8 zero[6] = {0, 0, 0, 0, 0, 0};
      nl_put_attr(b, IFLA_ADDRESS, zero, 6);
      nl_put_attr(b, IFLA_BROADCAST, zero, 6);
    } else {
      u8 bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
      nl_put_attr(b, IFLA_ADDRESS, tab[i].mac.bytes, 6);
      nl_put_attr(b, IFLA_BROADCAST, bcast, 6);
    }
    nl_msg_end(b, off);
  }
}

static void nl_emit_addr(struct nl_buf *b, u32 seq, u32 pid, u8 family,
                         u8 plen, u8 scope, int ifindex, const void *addr,
                         u16 alen, const char *label, const u8 *bcast) {
  usize off = nl_msg_begin(b, RTM_NEWADDR, NLM_F_MULTI, seq, pid);
  u8 ifa[8];
  memset(ifa, 0, sizeof(ifa));
  ifa[0] = family;
  ifa[1] = plen;
  ifa[2] = 0; /* ifa_flags */
  ifa[3] = scope;
  nl_store_u32(ifa + 4, (u32)ifindex);
  nl_put_bytes(b, ifa, sizeof(ifa));
  nl_put_attr(b, IFA_ADDRESS, addr, alen);
  nl_put_attr(b, IFA_LOCAL, addr, alen);
  if (bcast)
    nl_put_attr(b, IFA_BROADCAST, bcast, alen);
  if (label)
    nl_put_attr(b, IFA_LABEL, label, (u16)(strlen(label) + 1));
  nl_msg_end(b, off);
}

static void nl_dump_addrs(struct nl_buf *b, u32 seq, u32 pid, u8 family) {
  struct nl_iface tab[NL_MAX_IFACES];
  usize n = nl_iface_table(tab, NL_MAX_IFACES);
  int active = netdev_index_of(netdev_active());
  struct ipv4_addr ip = net_get_ip();
  struct ipv4_addr mask = net_get_netmask();
  u32 hmask = route_ipv4_to_host(mask);
  u8 plen = nl_mask_to_plen(hmask);

  for (usize i = 0; i < n; i++) {
    if (tab[i].loopback) {
      if (family == 0 || family == B1NIX_AF_INET) {
        u8 lo4[4] = {127, 0, 0, 1};
        nl_emit_addr(b, seq, pid, B1NIX_AF_INET, 8, RT_SCOPE_HOST,
                     tab[i].index, lo4, 4, "lo", 0);
      }
      if (family == 0 || family == B1NIX_AF_INET6) {
        u8 lo6[16] = {0};
        lo6[15] = 1;
        nl_emit_addr(b, seq, pid, B1NIX_AF_INET6, 128, RT_SCOPE_HOST,
                     tab[i].index, lo6, 16, 0, 0);
      }
      continue;
    }
    /* b1nix keeps one L3 configuration, owned by the active interface. An
     * interface with no address configured gets no RTM_NEWADDR at all —
     * that is what `ip addr` should show for a NIC DHCP has not bound. */
    if (tab[i].index != active)
      continue;
    if ((family == 0 || family == B1NIX_AF_INET) &&
        (ip.bytes[0] | ip.bytes[1] | ip.bytes[2] | ip.bytes[3])) {
      u32 hip = route_ipv4_to_host(ip);
      struct ipv4_addr bc = route_host_to_ipv4(hip | ~hmask);
      nl_emit_addr(b, seq, pid, B1NIX_AF_INET, plen, RT_SCOPE_UNIVERSE,
                   tab[i].index, ip.bytes, 4, tab[i].name, bc.bytes);
    }
    if (family == 0 || family == B1NIX_AF_INET6) {
      struct in6_addr_k ll = net_get_ip6_ll();
      if (!nl_in6_is_zero(ll.bytes))
        nl_emit_addr(b, seq, pid, B1NIX_AF_INET6, 64, RT_SCOPE_LINK,
                     tab[i].index, ll.bytes, 16, 0, 0);
      struct in6_addr_k g6 = net_get_ip6();
      if (!nl_in6_is_zero(g6.bytes))
        nl_emit_addr(b, seq, pid, B1NIX_AF_INET6, 64, RT_SCOPE_UNIVERSE,
                     tab[i].index, g6.bytes, 16, 0, 0);
    }
  }
}

static void nl_dump_routes(struct nl_buf *b, u32 seq, u32 pid, u8 family) {
  if (family == 0 || family == B1NIX_AF_INET) {
    struct route_info *r = kmalloc(sizeof(*r) * 64);
    if (r) {
      usize n = route_snapshot(r, 64);
      for (usize i = 0; i < n; i++) {
        usize off = nl_msg_begin(b, RTM_NEWROUTE, NLM_F_MULTI, seq, pid);
        u8 rtm[12];
        memset(rtm, 0, sizeof(rtm));
        rtm[0] = B1NIX_AF_INET;
        rtm[1] = nl_mask_to_plen(r[i].mask); /* rtm_dst_len */
        rtm[4] = (u8)r[i].table;
        rtm[5] = RTPROT_BOOT;
        rtm[6] = r[i].gateway ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
        rtm[7] = RTN_UNICAST;
        nl_put_bytes(b, rtm, sizeof(rtm));
        if (rtm[1]) {
          struct ipv4_addr d = route_host_to_ipv4(r[i].dst);
          nl_put_attr(b, RTA_DST, d.bytes, 4);
        }
        if (r[i].gateway) {
          struct ipv4_addr g = route_host_to_ipv4(r[i].gateway);
          nl_put_attr(b, RTA_GATEWAY, g.bytes, 4);
        }
        nl_put_attr_u32(b, RTA_OIF, (u32)(r[i].oif ? r[i].oif : 1));
        nl_put_attr_u32(b, RTA_TABLE, r[i].table);
        if (r[i].metric)
          nl_put_attr_u32(b, RTA_PRIORITY, r[i].metric);
        nl_msg_end(b, off);
      }
      kfree(r);
    }
  }
  if (family == 0 || family == B1NIX_AF_INET6) {
    struct route6_info *r6 = kmalloc(sizeof(*r6) * 64);
    if (r6) {
      usize n = route6_snapshot(r6, 64);
      for (usize i = 0; i < n; i++) {
        usize off = nl_msg_begin(b, RTM_NEWROUTE, NLM_F_MULTI, seq, pid);
        u8 rtm[12];
        memset(rtm, 0, sizeof(rtm));
        rtm[0] = B1NIX_AF_INET6;
        rtm[1] = r6[i].plen;
        rtm[4] = (u8)r6[i].table;
        rtm[5] = RTPROT_BOOT;
        rtm[6] = nl_in6_is_zero(r6[i].gateway) ? RT_SCOPE_LINK
                                               : RT_SCOPE_UNIVERSE;
        rtm[7] = RTN_UNICAST;
        nl_put_bytes(b, rtm, sizeof(rtm));
        if (r6[i].plen)
          nl_put_attr(b, RTA_DST, r6[i].dst, 16);
        if (!nl_in6_is_zero(r6[i].gateway))
          nl_put_attr(b, RTA_GATEWAY, r6[i].gateway, 16);
        nl_put_attr_u32(b, RTA_OIF, (u32)(r6[i].oif ? r6[i].oif : 1));
        nl_put_attr_u32(b, RTA_TABLE, r6[i].table);
        if (r6[i].metric)
          nl_put_attr_u32(b, RTA_PRIORITY, r6[i].metric);
        nl_msg_end(b, off);
      }
      kfree(r6);
    }
  }
}

static void nl_emit_neigh(struct nl_buf *b, u32 seq, u32 pid,
                          const struct neigh_info *e) {
  usize off = nl_msg_begin(b, RTM_NEWNEIGH, NLM_F_MULTI, seq, pid);
  u8 nd[12];
  memset(nd, 0, sizeof(nd));
  nd[0] = e->family;
  nl_store_u32(nd + 4, (u32)(e->oif ? e->oif : 1));
  nl_store_u16(nd + 8, e->permanent ? NUD_PERMANENT : NUD_REACHABLE);
  nd[11] = 0; /* ndm_type */
  nl_put_bytes(b, nd, sizeof(nd));
  nl_put_attr(b, NDA_DST, e->addr, e->addr_len);
  nl_put_attr(b, NDA_LLADDR, e->mac.bytes, 6);
  nl_msg_end(b, off);
}

/* Both families. IPv4 comes from the built-in ARP cache; IPv6 from ndp.ko
 * through the protocol registry, which reports nothing while that module is
 * not loaded. family == 0 (AF_UNSPEC) dumps everything, as `ip neigh` expects. */
static void nl_dump_neigh(struct nl_buf *b, u32 seq, u32 pid, u8 family) {
  struct neigh_info *e = kmalloc(sizeof(*e) * 64);
  if (!e)
    return;
  if (family == 0 || family == B1NIX_AF_INET) {
    usize n = arp_snapshot(e, 64);
    for (usize i = 0; i < n; i++)
      nl_emit_neigh(b, seq, pid, &e[i]);
  }
  if (family == 0 || family == B1NIX_AF_INET6) {
    usize n = ndp_dispatch_neigh_dump(e, 64);
    for (usize i = 0; i < n; i++)
      nl_emit_neigh(b, seq, pid, &e[i]);
  }
  kfree(e);
}

/* ── Attribute walking on the request side ──────────────────────────────── */

struct nl_attrs {
  const u8 *ptr[32];
  u16 len[32];
};

static void nl_parse_attrs(const u8 *p, usize len, struct nl_attrs *out) {
  memset(out, 0, sizeof(*out));
  while (len >= 4) {
    u16 alen = nl_load_u16(p);
    u16 atype = nl_load_u16(p + 2);
    if (alen < 4 || alen > len)
      break;
    if (atype < 32) {
      out->ptr[atype] = p + 4;
      out->len[atype] = (u16)(alen - 4);
    }
    usize step = nl_align(alen);
    if (step > len)
      break;
    p += step;
    len -= step;
  }
}

static u32 nl_attr_u32(const struct nl_attrs *a, u16 type, u32 def) {
  if (a->ptr[type] && a->len[type] >= 4)
    return nl_load_u32(a->ptr[type]);
  return def;
}

/* ── Request handlers that mutate state ─────────────────────────────────── */

static int nl_do_route(u16 type, const u8 *body, usize blen) {
  if (blen < 12)
    return -EINVAL;
  u8 family = body[0];
  u8 dst_len = body[1];
  u8 table = body[4];
  u16 metric_default = 0;
  struct nl_attrs a;
  nl_parse_attrs(body + 12, blen - 12, &a);
  u32 tbl = nl_attr_u32(&a, RTA_TABLE, table ? table : RT_TABLE_MAIN);
  if (tbl == 0)
    tbl = RT_TABLE_MAIN;
  int oif = (int)nl_attr_u32(&a, RTA_OIF, 0);
  u16 metric = (u16)nl_attr_u32(&a, RTA_PRIORITY, metric_default);

  if (family == B1NIX_AF_INET) {
    u32 dst = 0, gw = 0;
    if (a.ptr[RTA_DST] && a.len[RTA_DST] >= 4) {
      struct ipv4_addr d;
      memcpy(d.bytes, a.ptr[RTA_DST], 4);
      dst = route_ipv4_to_host(d);
    }
    if (a.ptr[RTA_GATEWAY] && a.len[RTA_GATEWAY] >= 4) {
      struct ipv4_addr g;
      memcpy(g.bytes, a.ptr[RTA_GATEWAY], 4);
      gw = route_ipv4_to_host(g);
    }
    u32 mask = nl_plen_to_mask(dst_len);
    dst &= mask;
    if (type == RTM_NEWROUTE) {
      u16 flags = RTF_UP;
      if (gw)
        flags |= RTF_GATEWAY;
      if (dst_len == 32)
        flags |= RTF_HOST;
      return route_add_table(dst, mask, gw, flags, metric, oif, tbl) == 0
                 ? 0
                 : -ENOBUFS;
    }
    return route_del_table(dst, mask, gw, tbl) == 0 ? 0 : -ESRCH;
  }

  if (family == B1NIX_AF_INET6) {
    struct in6_addr_k dst, gw;
    memset(&dst, 0, sizeof(dst));
    memset(&gw, 0, sizeof(gw));
    if (a.ptr[RTA_DST] && a.len[RTA_DST] >= 16)
      memcpy(dst.bytes, a.ptr[RTA_DST], 16);
    if (a.ptr[RTA_GATEWAY] && a.len[RTA_GATEWAY] >= 16)
      memcpy(gw.bytes, a.ptr[RTA_GATEWAY], 16);
    if (type == RTM_NEWROUTE) {
      u16 flags = RTF_UP;
      if (!nl_in6_is_zero(gw.bytes))
        flags |= RTF_GATEWAY;
      return route6_add_table(dst, dst_len, gw, flags, metric, oif, tbl) == 0
                 ? 0
                 : -ENOBUFS;
    }
    return route6_del_table(dst, dst_len, gw, tbl) == 0 ? 0 : -ESRCH;
  }
  return -EAFNOSUPPORT;
}

static int nl_do_neigh(u16 type, const u8 *body, usize blen) {
  if (blen < 12)
    return -EINVAL;
  u8 family = body[0];
  u16 state = nl_load_u16(body + 8);
  struct nl_attrs a;
  nl_parse_attrs(body + 12, blen - 12, &a);
  if (!a.ptr[NDA_DST])
    return -EINVAL;

  if (family == B1NIX_AF_INET) {
    if (a.len[NDA_DST] < 4)
      return -EINVAL;
    struct ipv4_addr ip;
    memcpy(ip.bytes, a.ptr[NDA_DST], 4);
    if (type == RTM_DELNEIGH)
      return arp_neigh_del(ip);
    if (!a.ptr[NDA_LLADDR] || a.len[NDA_LLADDR] < 6)
      return -EINVAL;
    struct mac_addr mac;
    memcpy(mac.bytes, a.ptr[NDA_LLADDR], 6);
    return arp_neigh_set(ip, mac, (state & NUD_PERMANENT) ? 1 : 0);
  }

  if (family == B1NIX_AF_INET6) {
    /* The IPv6 neighbour cache lives in ndp.ko and is reached through the
     * protocol registry; with that module absent the dispatchers report
     * EAFNOSUPPORT, which is the honest answer for a kernel that has no IPv6
     * neighbour cache at all. */
    if (a.len[NDA_DST] < 16)
      return -EINVAL;
    struct in6_addr_k ip;
    memcpy(ip.bytes, a.ptr[NDA_DST], 16);
    if (type == RTM_DELNEIGH)
      return ndp_dispatch_neigh_del(ip);
    if (!a.ptr[NDA_LLADDR] || a.len[NDA_LLADDR] < 6)
      return -EINVAL;
    struct mac_addr mac;
    memcpy(mac.bytes, a.ptr[NDA_LLADDR], 6);
    return ndp_dispatch_neigh_set(ip, mac, (state & NUD_PERMANENT) ? 1 : 0);
  }
  return -EAFNOSUPPORT;
}

/* RTM_NEWLINK/RTM_SETLINK: `ip link set <if> up|down`. The ifinfomsg carries
 * the wanted flags and a change mask; b1nix implements IFF_UP and refuses any
 * other bit rather than silently ignoring it. */
static int nl_do_setlink(const u8 *body, usize blen) {
  if (blen < 16)
    return -EINVAL;
  int index = (int)nl_load_u32(body + 4);
  u32 flags = nl_load_u32(body + 8);
  u32 change = nl_load_u32(body + 12);
  if (change == 0)
    return -EINVAL; /* nothing asked for */
  if (change & ~(u32)NL_IFF_UP)
    return -EOPNOTSUPP;
  if (!cred_has_cap(scheduler_get_current_cred(), CAP_NET_ADMIN))
    return -EPERM;
  if (index == NETLINK_LO_IFINDEX)
    /* Loopback is up for as long as the kernel runs; taking it down would
     * break every local socket and b1nix has no way to honour it. */
    return -EOPNOTSUPP;
  struct netdev *nd = netdev_by_index(index);
  if (!nd)
    return -ENODEV;
  return netdev_set_admin_up(nd, (flags & NL_IFF_UP) ? 1 : 0);
}

static int nl_do_addr(u16 type, const u8 *body, usize blen) {
  if (blen < 8)
    return -EINVAL;
  u8 family = body[0];
  u8 plen = body[1];
  int ifindex = (int)nl_load_u32(body + 4);
  struct nl_attrs a;
  nl_parse_attrs(body + 8, blen - 8, &a);
  const u8 *addr = a.ptr[IFA_LOCAL] ? a.ptr[IFA_LOCAL] : a.ptr[IFA_ADDRESS];
  u16 alen = a.ptr[IFA_LOCAL] ? a.len[IFA_LOCAL] : a.len[IFA_ADDRESS];

  struct nl_iface iface;
  if (ifindex && !nl_iface_by_index(ifindex, &iface))
    return -ENODEV;
  /* b1nix owns exactly one L3 configuration and it belongs to the active
   * interface; loopback's addresses are constants, not configuration. */
  if (ifindex == NETLINK_LO_IFINDEX)
    return -EOPNOTSUPP;
  int active = netdev_index_of(netdev_active());
  if (!active)
    return -ENODEV;
  if (ifindex && ifindex != active)
    return -EOPNOTSUPP;

  if (family == B1NIX_AF_INET) {
    if (!addr || alen < 4)
      return -EINVAL;
    if (type == RTM_DELADDR) {
      struct ipv4_addr cur = net_get_ip();
      if (memcmp(cur.bytes, addr, 4) != 0)
        return -EADDRNOTAVAIL;
      struct ipv4_addr zero = {{0, 0, 0, 0}};
      net_set_ip(zero);
      net_set_netmask(zero);
      route_flush_dynamic();
      return 0;
    }
    struct ipv4_addr ip;
    memcpy(ip.bytes, addr, 4);
    struct ipv4_addr mask = route_host_to_ipv4(nl_plen_to_mask(plen));
    net_set_ip(ip);
    net_set_netmask(mask);
    route_configure_interface(ip, mask, net_get_gateway());
    return 0;
  }
  if (family == B1NIX_AF_INET6) {
    if (!addr || alen < 16)
      return -EINVAL;
    struct in6_addr_k a6;
    memcpy(a6.bytes, addr, 16);
    if (type == RTM_DELADDR) {
      struct in6_addr_k cur = net_get_ip6();
      if (memcmp(cur.bytes, a6.bytes, 16) != 0)
        return -EADDRNOTAVAIL;
      struct in6_addr_k zero;
      memset(&zero, 0, sizeof(zero));
      net_set_ip6(zero);
      return 0;
    }
    net_set_ip6(a6);
    return 0;
  }
  return -EAFNOSUPPORT;
}

/* ── Queue plumbing ─────────────────────────────────────────────────────── */

static void netlink_enqueue(struct vfs_socket_state *s, const u8 *data,
                            usize len) {
  if (s->udp_q_count >= 8)
    return;
  u8 slot = s->udp_q_tail;
  usize copy = len > NL_SLOT_MAX ? NL_SLOT_MAX : len;
  memcpy(s->udp_q_buf[slot], data, copy);
  s->udp_q_len[slot] = copy;
  s->udp_q_tail = (u8)((s->udp_q_tail + 1) % 8);
  s->udp_q_count++;
  s->recv_len = s->udp_q_len[s->udp_q_head];
  scheduler_wake_all(s);
  scheduler_wake_all(vfs_poll_chan);
}

/* Chop a finished multipart reply into datagrams on nlmsghdr boundaries so no
 * message is ever split across two recvmsg() calls — a netlink reader parses
 * each datagram independently and a torn header would desynchronise it. */
static void netlink_deliver(struct vfs_socket_state *s, const u8 *buf,
                            usize len) {
  usize pos = 0;
  while (pos < len) {
    usize chunk = 0;
    while (pos + chunk + 16 <= len) {
      u32 mlen = nl_load_u32(buf + pos + chunk);
      if (mlen < 16 || pos + chunk + mlen > len)
        break;
      usize step = nl_align(mlen);
      if (pos + chunk + step > len)
        step = mlen;
      if (chunk + step > NL_SLOT_MAX)
        break;
      chunk += step;
    }
    if (chunk == 0) /* a single message larger than a slot: cannot happen with
                     * the encoders above, but never loop forever. */
      break;
    netlink_enqueue(s, buf + pos, chunk);
    pos += chunk;
  }
}

/* NLMSG_ERROR: {nlmsghdr, int error, copy of the offending nlmsghdr}. */
static void nl_put_error(struct nl_buf *b, u32 seq, u32 pid, int err,
                         const u8 *req_hdr) {
  usize off = nl_msg_begin(b, NLMSG_ERROR, 0, seq, pid);
  u8 e[4];
  nl_store_u32(e, (u32)(i32)err);
  nl_put_bytes(b, e, 4);
  nl_put_bytes(b, req_hdr, 16);
  nl_msg_end(b, off);
}

static void nl_put_done(struct nl_buf *b, u32 seq, u32 pid) {
  usize off = nl_msg_begin(b, NLMSG_DONE, NLM_F_MULTI, seq, pid);
  u8 z[4] = {0, 0, 0, 0};
  nl_put_bytes(b, z, 4);
  nl_msg_end(b, off);
}

/* ── Entry point ────────────────────────────────────────────────────────── */

isize netlink_socket_send(struct vfs_socket_state *s, const void *buf,
                          usize len) {
  if (!s || !buf || len < 16)
    return -EINVAL;

  u8 *out = kmalloc(NL_DUMP_MAX);
  if (!out)
    return -ENOMEM;
  struct nl_buf b = {out, NL_DUMP_MAX, 0, 0};

  const u8 *p = (const u8 *)buf;
  usize left = len;
  while (left >= 16) {
    u32 mlen = nl_load_u32(p);
    if (mlen < 16 || mlen > left)
      break;
    u16 type = nl_load_u16(p + 4);
    u16 flags = nl_load_u16(p + 6);
    u32 seq = nl_load_u32(p + 8);
    u32 pid = nl_load_u32(p + 12);
    const u8 *body = p + 16;
    usize blen = mlen - 16;
    /* The family selector a dump request carries: RTM_GET* is sent with either
     * a struct rtgenmsg (1 byte) or the full ifinfomsg/rtmsg; in every case
     * byte 0 is the address family, and 0 (AF_UNSPEC) means "everything". */
    u8 family = blen >= 1 ? body[0] : 0;
    if (family != B1NIX_AF_INET && family != B1NIX_AF_INET6)
      family = 0;

    int err = 0;
    int is_dump = (flags & NLM_F_DUMP) == NLM_F_DUMP;
    u32 rpid = nl_sock_pid(s);
    /* A request may name its own port id; prefer it when the socket is
     * unbound, so an unbound sender still recognises its replies. */
    if (rpid == 0)
      rpid = pid;

    switch (type) {
    case RTM_GETLINK:
      if (is_dump) {
        nl_dump_links(&b, seq, rpid, family);
        nl_put_done(&b, seq, rpid);
      } else {
        nl_dump_links(&b, seq, rpid, family);
      }
      break;
    case RTM_GETADDR:
      nl_dump_addrs(&b, seq, rpid, family);
      if (is_dump)
        nl_put_done(&b, seq, rpid);
      break;
    case RTM_GETROUTE:
      nl_dump_routes(&b, seq, rpid, family);
      if (is_dump)
        nl_put_done(&b, seq, rpid);
      break;
    case RTM_GETNEIGH:
      nl_dump_neigh(&b, seq, rpid, family);
      if (is_dump)
        nl_put_done(&b, seq, rpid);
      break;
    case RTM_NEWROUTE:
    case RTM_DELROUTE:
      err = nl_do_route(type, body, blen);
      if (err || (flags & NLM_F_ACK))
        nl_put_error(&b, seq, rpid, err, p);
      break;
    case RTM_NEWNEIGH:
    case RTM_DELNEIGH:
      err = nl_do_neigh(type, body, blen);
      if (err || (flags & NLM_F_ACK))
        nl_put_error(&b, seq, rpid, err, p);
      break;
    case RTM_NEWADDR:
    case RTM_DELADDR:
      err = nl_do_addr(type, body, blen);
      if (err || (flags & NLM_F_ACK))
        nl_put_error(&b, seq, rpid, err, p);
      break;
    case RTM_NEWLINK:
    case RTM_SETLINK:
      /* IFF_UP is the one link property b1nix can really change; creating or
       * renaming an interface is still not something this kernel does, and
       * saying so beats accepting a change that would not happen. */
      nl_put_error(&b, seq, rpid, nl_do_setlink(body, blen), p);
      break;
    case RTM_DELLINK:
      /* Interfaces exist for exactly as long as their driver is registered. */
      nl_put_error(&b, seq, rpid, -EOPNOTSUPP, p);
      break;
    default:
      nl_put_error(&b, seq, rpid, -EOPNOTSUPP, p);
      break;
    }

    usize step = nl_align(mlen);
    if (step > left)
      break;
    p += step;
    left -= step;
  }

  if (b.len)
    netlink_deliver(s, out, b.len);
  kfree(out);
  return (isize)len;
}
