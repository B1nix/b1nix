/* AF_PACKET — ethernet frames as they appear on the wire.
 *
 * A packet socket is the one way a program can see or send a frame the IP
 * stack has no opinion about: udhcpc has to broadcast a DHCPDISCOVER from
 * 0.0.0.0 before it owns an address, arping wants to write its own ARP, and
 * tcpdump wants everything. All three ask for AF_PACKET.
 *
 * Only the five things a family really has to provide live here — create,
 * bind, send, close and the receive tap. Receive, poll and select come from
 * the generic socket layer: a packet socket queues into the same per-socket
 * datagram ring UDP and netlink use (vfs_socket_recv_h / socket_poll already
 * serve SOCK_DGRAM and SOCK_RAW), so there is no packet-specific read path.
 */

#include <b1nix/namespace.h>
#include <b1nix/packet.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/vfs.h>
#include <b1nix/sched.h>
#include <b1nix/errno.h>
#include <b1nix/posix.h>
#include <string.h>

/* Same ceiling as the raw-ICMP and netlink registries: a handful of listeners
 * is all anything on this system opens, and a fixed table keeps the RX tap
 * (which runs from the net poll task) allocation-free. */
#define MAX_PACKET_SOCKS 8
static struct vfs_socket_state *packet_socks[MAX_PACKET_SOCKS];

/* The queue slot geometry the generic recv path uses. */
#define PKT_Q_SLOTS 8

void packet_sock_register(struct vfs_socket_state *s) {
  for (int i = 0; i < MAX_PACKET_SOCKS; i++)
    if (!packet_socks[i]) {
      packet_socks[i] = s;
      return;
    }
}

void packet_sock_unregister(struct vfs_socket_state *s) {
  for (int i = 0; i < MAX_PACKET_SOCKS; i++)
    if (packet_socks[i] == s)
      packet_socks[i] = 0;
}

/* Host-order ethertype this socket wants, or 0 for "every frame".
 * sll_protocol and the protocol argument to socket(2) are both in network
 * byte order (userspace writes htons(ETH_P_ARP)), so they are byte-swapped
 * here rather than at every comparison. */
static u16 packet_filter_proto(const struct vfs_socket_state *s) {
  u16 net_order = s->bound ? s->local.ll.sll_protocol : (u16)s->protocol;
  u16 host = (u16)(((net_order & 0xFF) << 8) | ((net_order >> 8) & 0xFF));
  if (host == B1NIX_ETH_P_ALL)
    return 0;
  return host;
}

int packet_bind(struct vfs_socket_state *s, const struct b1nix_sockaddr_ll *sll,
                usize addrlen) {
  /* Linux accepts a short sockaddr_ll as long as the family, protocol and
   * ifindex are there — udhcpc passes exactly sizeof(struct sockaddr_ll), but
   * a caller that stops after sll_ifindex is still asking something
   * unambiguous, so accept it and leave sll_addr empty. */
  if (!sll || addrlen < 8)
    return -EINVAL;
  if (sll->sll_ifindex != 0 && !netdev_by_index(sll->sll_ifindex))
    return -ENODEV;

  struct b1nix_sockaddr_ll bound;
  memset(&bound, 0, sizeof(bound));
  bound.sll_family = B1NIX_AF_PACKET;
  bound.sll_protocol = sll->sll_protocol;
  bound.sll_ifindex = sll->sll_ifindex;
  bound.sll_hatype = B1NIX_ARPHRD_ETHER;
  bound.sll_halen = 6;
  struct netdev *nd = sll->sll_ifindex ? netdev_by_index(sll->sll_ifindex)
                                       : netdev_active();
  if (nd)
    memcpy(bound.sll_addr, nd->mac.bytes, 6);
  memcpy(&s->local.ll, &bound, sizeof(bound));
  s->bound = 1;
  return 0;
}

/* The interface a send should leave by: the sendto address if it named one,
 * otherwise the bind, otherwise the active NIC. */
static struct netdev *packet_tx_dev(struct vfs_socket_state *s) {
  int idx = 0;
  if (s->peer.ll.sll_family == B1NIX_AF_PACKET && s->peer.ll.sll_ifindex)
    idx = s->peer.ll.sll_ifindex;
  else if (s->bound && s->local.ll.sll_ifindex)
    idx = s->local.ll.sll_ifindex;
  struct netdev *nd = idx ? netdev_by_index(idx) : netdev_active();
  return nd;
}

isize packet_send(struct vfs_socket_state *s, const void *buf, usize len) {
  struct netdev *nd = packet_tx_dev(s);
  if (!nd)
    return -ENODEV;
  if (!netdev_is_admin_up(nd))
    return -ENETDOWN;
  if (!nd->transmit)
    return -EOPNOTSUPP;

  if (s->type == B1NIX_SOCK_RAW) {
    /* The caller wrote the header itself; put the bytes on the wire as they
     * are. net_send_ethernet_tx() cannot be used here — it builds its own
     * header and overrides the source MAC, which is precisely the freedom a
     * SOCK_RAW packet socket is opened for. */
    if (len < 14)
      return -EINVAL;
    const u8 *frame = (const u8 *)buf;
    int rc = nd->transmit(nd, frame, frame + 14, len - 14, 0);
    if (rc == 0)
      packet_socket_tx(nd, frame, frame + 14, len - 14);
    return rc < 0 ? (isize)rc : (isize)len;
  }

  /* SOCK_DGRAM: the kernel supplies the header from the address. */
  const struct b1nix_sockaddr_ll *dst =
      (s->peer.ll.sll_family == B1NIX_AF_PACKET) ? &s->peer.ll : &s->local.ll;
  if (dst->sll_halen != 6)
    return -EINVAL;
  struct mac_addr mac;
  memcpy(mac.bytes, dst->sll_addr, 6);
  u16 ethertype = (u16)(((dst->sll_protocol & 0xFF) << 8) |
                        ((dst->sll_protocol >> 8) & 0xFF));
  net_send_ethernet_tx(nd, mac, ethertype, buf, len, 0);
  return (isize)len;
}

/* Queue one frame on one socket. The per-datagram metadata the generic recv
 * path already carries (udp_q_src_ip / udp_q_src_port) is reused to describe
 * the link-layer source: bytes 0..5 the source MAC, byte 6 the packet type,
 * bytes 7..8 the ethertype in network order, and the port field the ifindex.
 * Nothing else needs a new field in struct vfs_socket_state. */
static void packet_enqueue(struct vfs_socket_state *s, int ifindex,
                           const u8 *hdr, const void *payload,
                           usize payload_len, u8 pkttype) {
  if (s->udp_q_count >= PKT_Q_SLOTS)
    return; /* the reader is behind: drop, as a packet socket is allowed to */

  u8 slot = s->udp_q_tail;
  usize cap = sizeof(s->udp_q_buf[slot]);
  usize n = 0;
  if (s->type == B1NIX_SOCK_RAW) {
    /* SOCK_RAW gets the frame as it goes on the wire, header included. */
    n = cap < 14 ? cap : 14;
    memcpy(s->udp_q_buf[slot], hdr, n);
  }
  /* SOCK_DGRAM gets only the payload; the header is reported in the address. */
  usize room = cap - n;
  usize copy = payload_len > room ? room : payload_len;
  memcpy(s->udp_q_buf[slot] + n, payload, copy);
  s->udp_q_len[slot] = n + copy;

  memset(s->udp_q_src_ip[slot], 0, sizeof(s->udp_q_src_ip[slot]));
  memcpy(s->udp_q_src_ip[slot], hdr + 6, 6); /* source MAC */
  s->udp_q_src_ip[slot][6] = pkttype;
  s->udp_q_src_ip[slot][7] = hdr[12];
  s->udp_q_src_ip[slot][8] = hdr[13];
  s->udp_q_src_port[slot] = (u16)ifindex;
  s->udp_q_src_is6[slot] = 0;

  s->udp_q_tail = (u8)((s->udp_q_tail + 1) % PKT_Q_SLOTS);
  s->udp_q_count++;
  s->recv_len = s->udp_q_len[s->udp_q_head];
  scheduler_wake_all(s);
  scheduler_wake_all(vfs_poll_chan);
}

/* Deliver one frame to every socket that asked for it. Header and payload are
 * separate because the transmit path has them that way (struct netdev's
 * ->transmit takes them separately to keep a frame buffer off a deep send
 * stack) and reassembling one just to split it again would be a copy for
 * nothing. */
static void packet_deliver(struct netdev *dev, const u8 *hdr,
                           const void *payload, usize payload_len,
                           u8 pkttype) {
  int ifindex = dev ? netdev_index_of(dev) : 0;
  u16 ethertype = (u16)((hdr[12] << 8) | hdr[13]);

  for (int i = 0; i < MAX_PACKET_SOCKS; i++) {
    struct vfs_socket_state *s = packet_socks[i];
    if (!s)
      continue;
    if (s->shut_rd)
      continue;
    /* A packet socket sees its own network namespace and nothing else: an
     * ETH_P_ALL tap would otherwise read every namespace's traffic, which is
     * the one guarantee the boundary has to make. */
    if (dev && s->netns != dev->netns)
      continue;
    /* A bind to an interface is a filter, not just a send default. */
    if (s->bound && s->local.ll.sll_ifindex &&
        s->local.ll.sll_ifindex != ifindex)
      continue;
    u16 want = packet_filter_proto(s);
    if (want && want != ethertype)
      continue;
    packet_enqueue(s, ifindex, hdr, payload, payload_len, pkttype);
  }
}

void packet_socket_rx(struct netdev *rx, const void *frame, usize len) {
  if (len < 14)
    return;
  const u8 *f = (const u8 *)frame;

  /* Who the frame was addressed to, which is what sll_pkttype reports. */
  u8 pkttype = B1NIX_PACKET_HOST;
  int all_ones = 1;
  for (int i = 0; i < 6; i++)
    if (f[i] != 0xFF)
      all_ones = 0;
  if (all_ones)
    pkttype = B1NIX_PACKET_BROADCAST;
  else if (f[0] & 0x01)
    pkttype = B1NIX_PACKET_MULTICAST;
  else if (rx && memcmp(f, rx->mac.bytes, 6) != 0)
    pkttype = B1NIX_PACKET_OTHERHOST;

  packet_deliver(rx, f, f + 14, len - 14, pkttype);
}

/* Outgoing frames are part of what a packet socket sees — that is why tcpdump
 * shows both directions, and why a program can watch what it sent itself.
 * Linux does this in dev_queue_xmit_nit; here it is the one call every
 * transmit path already funnels through. */
void packet_socket_tx(struct netdev *dev, const u8 *hdr, const void *payload,
                      usize payload_len) {
  packet_deliver(dev, hdr, payload, payload_len, B1NIX_PACKET_OUTGOING);
}

usize packet_fill_src(struct vfs_socket_state *s, void *addr, usize cap) {
  if (!addr || !cap)
    return 0;
  struct b1nix_sockaddr_ll sll;
  memset(&sll, 0, sizeof(sll));
  sll.sll_family = B1NIX_AF_PACKET;
  sll.sll_hatype = B1NIX_ARPHRD_ETHER;
  sll.sll_halen = 6;
  if (s->udp_last_src_valid) {
    memcpy(sll.sll_addr, s->udp_last_src_ip, 6);
    sll.sll_pkttype = s->udp_last_src_ip[6];
    sll.sll_protocol =
        (u16)(s->udp_last_src_ip[7] | (s->udp_last_src_ip[8] << 8));
    sll.sll_ifindex = (i32)s->udp_last_src_port;
  }
  usize n = cap < sizeof(sll) ? cap : sizeof(sll);
  memcpy(addr, &sll, n);
  return n;
}
