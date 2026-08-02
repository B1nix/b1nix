#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <stdlib.h>
#include <string.h>
#include <b1nix/net.h>
#include <b1nix/netdev.h>
#include <b1nix/syscall.h>

struct udp_binding {
  int used;
  u16 port;
  struct vfs_handle *handle;
};
#define MAX_UDP_BINDINGS 128
struct udp_binding udp_bindings[MAX_UDP_BINDINGS];

static u16 ntoh16(u16 value) {
  return (u16)((value << 8) | (value >> 8));
}

/* IPv4-mapped IPv6 address ::ffff:a.b.c.d (dual-stack): the first 10 bytes are
 * zero, then 0xffff, then the 4 IPv4 octets. */
static int in6_is_v4mapped(const struct in6_addr_k *a) {
  for (int i = 0; i < 10; i++)
    if (a->bytes[i] != 0)
      return 0;
  return a->bytes[10] == 0xff && a->bytes[11] == 0xff;
}

/* Bridge functions to avoid including private net headers in vfs.h */
/* ── Raw ICMP sockets (BusyBox ping) ──
 * A small registry of SOCK_RAW/IPPROTO_ICMP sockets. icmp_receive() delivers
 * each ICMP packet to every raw socket, wrapped in a synthetic IPv4 header
 * (SOCK_RAW readers expect the IP header included). Recv/poll reuse the
 * datagram queue. */
#define MAX_RAW_SOCKS 8
static struct vfs_socket_state *raw_socks[MAX_RAW_SOCKS];

static void raw_sock_register(struct vfs_socket_state *s) {
  for (int i = 0; i < MAX_RAW_SOCKS; i++)
    if (!raw_socks[i]) {
      raw_socks[i] = s;
      return;
    }
}

static void raw_sock_unregister(struct vfs_socket_state *s) {
  for (int i = 0; i < MAX_RAW_SOCKS; i++)
    if (raw_socks[i] == s)
      raw_socks[i] = 0;
}

void vfs_socket_push_raw_icmp(struct ipv4_addr src, const void *icmp,
                              usize len) {
  static u8 pkt[2048];
  usize iphl = 20;
  if (len > sizeof(pkt) - iphl)
    len = sizeof(pkt) - iphl;
  memset(pkt, 0, iphl);
  pkt[0] = 0x45; /* IPv4, IHL=5 */
  u16 tot = (u16)(iphl + len);
  pkt[2] = (u8)(tot >> 8);
  pkt[3] = (u8)(tot & 0xFF);
  pkt[8] = 64; /* TTL */
  pkt[9] = 1;  /* protocol = ICMP */
  struct ipv4_addr myip = net_get_ip();
  pkt[12] = src.bytes[0];
  pkt[13] = src.bytes[1];
  pkt[14] = src.bytes[2];
  pkt[15] = src.bytes[3];
  pkt[16] = myip.bytes[0];
  pkt[17] = myip.bytes[1];
  pkt[18] = myip.bytes[2];
  pkt[19] = myip.bytes[3];
  memcpy(pkt + iphl, icmp, len);
  usize total = iphl + len;
  for (int i = 0; i < MAX_RAW_SOCKS; i++) {
    struct vfs_socket_state *s = raw_socks[i];
    if (!s || s->udp_q_count >= 8)
      continue;
    u8 slot = s->udp_q_tail;
    usize copy =
        total > sizeof(s->udp_q_buf[slot]) ? sizeof(s->udp_q_buf[slot]) : total;
    memcpy(s->udp_q_buf[slot], pkt, copy);
    s->udp_q_len[slot] = copy;
    s->udp_q_tail = (u8)((s->udp_q_tail + 1) % 8);
    s->udp_q_count++;
    s->recv_len = s->udp_q_len[s->udp_q_head];
    scheduler_wake_all(s);
    scheduler_wake_all(vfs_poll_chan);
  }
}

/* ── Minimal rtnetlink (AF_NETLINK) for BusyBox `ip` ──
 * `ip` speaks rtnetlink exclusively. We model a single interface "eth0" and
 * answer the three dump requests it issues. A dump request sent on the socket
 * is answered synchronously: the encoded reply is enqueued to the socket's
 * datagram queue, so the following recvmsg() returns it. */
#define NL_RTM_GETLINK  18
#define NL_RTM_NEWLINK  16
#define NL_RTM_GETADDR  22
#define NL_RTM_NEWADDR  20
#define NL_RTM_GETROUTE 26
#define NL_RTM_NEWROUTE 24
#define NL_NLMSG_DONE   3
#define NL_NLM_F_MULTI  2
#define NL_ARPHRD_ETHER 1
#define NL_IFF_UP_RUN_BC 0x43 /* UP|BROADCAST|RUNNING */

static usize nl_align(usize n) { return (n + 3u) & ~3u; }

/* Append an rtattr {len, type, data} at p, return bytes consumed (aligned). */
static usize nl_put_attr(u8 *p, u16 type, const void *data, u16 dlen) {
  u16 rta_len = (u16)(4 + dlen);
  p[0] = (u8)(rta_len & 0xFF);
  p[1] = (u8)(rta_len >> 8);
  p[2] = (u8)(type & 0xFF);
  p[3] = (u8)(type >> 8);
  if (dlen)
    memcpy(p + 4, data, dlen);
  return nl_align(rta_len);
}

static void nl_put_u32(u8 *p, u32 v) {
  p[0] = (u8)v;
  p[1] = (u8)(v >> 8);
  p[2] = (u8)(v >> 16);
  p[3] = (u8)(v >> 24);
}

/* Build the dump reply for `rtm_type` into out[]; returns total length. */
static usize netlink_build_dump(int rtm_type, u32 seq, u8 *out, usize cap) {
  usize off = 0;
  u8 *msg;       /* start of the current nlmsghdr */
  usize body;    /* offset just past the nlmsghdr */
  struct ipv4_addr ip = net_get_ip();
  struct mac_addr mac = net_get_mac();

  if (off + 64 > cap)
    return 0;
  msg = out + off;
  body = off + 16; /* nlmsghdr is 16 bytes */

  if (rtm_type == NL_RTM_GETLINK) {
    u8 *b = out + body;
    usize a = 0;
    memset(b, 0, 16);            /* ifinfomsg */
    b[2] = NL_ARPHRD_ETHER;      /* ifi_type */
    nl_put_u32(b + 4, 1);        /* ifi_index = 1 */
    nl_put_u32(b + 8, NL_IFF_UP_RUN_BC); /* ifi_flags */
    a = 16;
    a += nl_put_attr(b + a, 3 /*IFLA_IFNAME*/, "eth0", 5);
    u32 mtu = 1500;
    a += nl_put_attr(b + a, 4 /*IFLA_MTU*/, &mtu, 4);
    a += nl_put_attr(b + a, 1 /*IFLA_ADDRESS*/, mac.bytes, 6);
    off = body + a;
  } else if (rtm_type == NL_RTM_GETADDR) {
    u8 *b = out + body;
    usize a = 0;
    memset(b, 0, 8);             /* ifaddrmsg */
    b[0] = B1NIX_AF_INET;        /* ifa_family */
    b[1] = 24;                   /* ifa_prefixlen */
    b[3] = 0;                    /* ifa_scope = global */
    nl_put_u32(b + 4, 1);        /* ifa_index = 1 */
    a = 8;
    a += nl_put_attr(b + a, 2 /*IFA_LOCAL*/, ip.bytes, 4);
    a += nl_put_attr(b + a, 1 /*IFA_ADDRESS*/, ip.bytes, 4);
    a += nl_put_attr(b + a, 3 /*IFA_LABEL*/, "eth0", 5);
    off = body + a;
  } else if (rtm_type == NL_RTM_GETROUTE) {
    u8 *b = out + body;
    usize a = 0;
    memset(b, 0, 12);            /* rtmsg */
    b[0] = B1NIX_AF_INET;        /* rtm_family */
    b[1] = 24;                   /* rtm_dst_len */
    b[4] = 254;                  /* rtm_table = RT_TABLE_MAIN */
    b[5] = 3;                    /* rtm_protocol = RTPROT_BOOT */
    b[6] = 253;                  /* rtm_scope = RT_SCOPE_LINK */
    b[7] = 1;                    /* rtm_type = RTN_UNICAST */
    a = 12;
    struct ipv4_addr net = {{ip.bytes[0], ip.bytes[1], ip.bytes[2], 0}};
    a += nl_put_attr(b + a, 1 /*RTA_DST*/, net.bytes, 4);
    u32 oif = 1;
    a += nl_put_attr(b + a, 4 /*RTA_OIF*/, &oif, 4);
    off = body + a;
  } else {
    return 0;
  }

  /* Back-fill the first nlmsghdr: len, type=NEW*, flags=MULTI, seq, pid=0. */
  u16 newtype = (rtm_type == NL_RTM_GETLINK)   ? NL_RTM_NEWLINK
                : (rtm_type == NL_RTM_GETADDR) ? NL_RTM_NEWADDR
                                               : NL_RTM_NEWROUTE;
  u32 mlen = (u32)(off - (usize)(msg - out));
  nl_put_u32(msg + 0, mlen);
  msg[4] = (u8)(newtype & 0xFF);
  msg[5] = (u8)(newtype >> 8);
  msg[6] = (u8)(NL_NLM_F_MULTI & 0xFF);
  msg[7] = (u8)(NL_NLM_F_MULTI >> 8);
  nl_put_u32(msg + 8, seq);
  nl_put_u32(msg + 12, 0);

  /* NLMSG_DONE terminator: nlmsghdr(16) + int(0). */
  off = nl_align(off);
  if (off + 20 > cap)
    return off;
  u8 *d = out + off;
  nl_put_u32(d + 0, 20);
  d[4] = (u8)(NL_NLMSG_DONE & 0xFF);
  d[5] = (u8)(NL_NLMSG_DONE >> 8);
  d[6] = (u8)(NL_NLM_F_MULTI & 0xFF);
  d[7] = (u8)(NL_NLM_F_MULTI >> 8);
  nl_put_u32(d + 8, seq);
  nl_put_u32(d + 12, 0);
  nl_put_u32(d + 16, 0); /* done code */
  off += 20;
  return off;
}

/* Enqueue a ready datagram into a socket's recv queue. */
static void netlink_enqueue(struct vfs_socket_state *s, const u8 *data,
                            usize len) {
  if (s->udp_q_count >= 8)
    return;
  u8 slot = s->udp_q_tail;
  usize copy = len > sizeof(s->udp_q_buf[slot]) ? sizeof(s->udp_q_buf[slot]) : len;
  memcpy(s->udp_q_buf[slot], data, copy);
  s->udp_q_len[slot] = copy;
  s->udp_q_tail = (u8)((s->udp_q_tail + 1) % 8);
  s->udp_q_count++;
  s->recv_len = s->udp_q_len[s->udp_q_head];
  scheduler_wake_all(s);
  scheduler_wake_all(vfs_poll_chan);
}

static isize netlink_send(struct vfs_socket_state *s, const void *buf,
                          usize len) {
  if (len < 16)
    return -EINVAL;
  const u8 *p = (const u8 *)buf;
  u16 type = (u16)(p[4] | (p[5] << 8));
  u32 seq = (u32)(p[8] | (p[9] << 8) | (p[10] << 16) | (p[11] << 24));
  static u8 reply[2048];
  usize n = netlink_build_dump(type, seq, reply, sizeof(reply));
  if (n)
    netlink_enqueue(s, reply, n);
  return (isize)len;
}

isize vfs_socket_send_h(struct vfs_handle *h, const void *buf, usize len, int flags) {
  (void)flags;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;

  /* After shutdown(SHUT_WR) the write half is closed: POSIX requires EPIPE. */
  if (s->shut_wr)
    return -EPIPE;

  if (s->domain == B1NIX_AF_NETLINK)
    return netlink_send(s, buf, len);

  if (s->domain == B1NIX_AF_UNIX) {
    return unix_send(s, buf, len, (h->flags & B1NIX_O_NONBLOCK) ||
                                      (flags & B1NIX_MSG_DONTWAIT));
  }

  if (s->domain == B1NIX_AF_INET6) {
    if (s->type == B1NIX_SOCK_DGRAM) {
      if (!s->connected && s->peer.in6.sin6_port == 0)
        return -ENOTCONN;
      struct in6_addr_k dst;
      memcpy(dst.bytes, s->peer.in6.sin6_addr.s6_addr, 16);
      if (in6_is_v4mapped(&dst)) {
        if (s->ipv6_v6only)
          return -EAFNOSUPPORT;
        /* Dual-stack: ::ffff:a.b.c.d is delivered over the IPv4 path. */
        struct ipv4_addr v4 = {{dst.bytes[12], dst.bytes[13], dst.bytes[14],
                                dst.bytes[15]}};
        udp_send_net(v4, s->local.in6.sin6_port, s->peer.in6.sin6_port, buf,
                     len);
        return (isize)len;
      }
      udp6_send(dst, s->local.in6.sin6_port, s->peer.in6.sin6_port, buf, len);
      return (isize)len;
    }
    if (s->type == B1NIX_SOCK_STREAM && s->tcp_conn) {
      if (!s->connected && tcp_is_established((struct tcp_conn *)s->tcp_conn))
        s->connected = 1;
      if (!s->connected)
        return -EAGAIN;
      return tcp_send((struct tcp_conn *)s->tcp_conn, buf, len);
    }
    return -ENOTCONN;
  }

  struct ipv4_addr dst_ip;
  dst_ip.bytes[0] = s->peer.in.sin_addr & 0xFF;
  dst_ip.bytes[1] = (s->peer.in.sin_addr >> 8) & 0xFF;
  dst_ip.bytes[2] = (s->peer.in.sin_addr >> 16) & 0xFF;
  dst_ip.bytes[3] = (s->peer.in.sin_addr >> 24) & 0xFF;

  if (s->type == B1NIX_SOCK_RAW) {
    /* The payload is a complete L4 packet (BusyBox ping builds the ICMP
     * header). Wrap it in IPv4 with the socket's protocol and ship it. */
    ipv4_send(dst_ip, (u8)(s->protocol ? s->protocol : 1), buf, len);
    return (isize)len;
  }
  if (s->type == B1NIX_SOCK_DGRAM) {
    if (!s->connected && s->peer.in.sin_port == 0)
      return -ENOTCONN;
    udp_send_net(dst_ip, s->local.in.sin_port, s->peer.in.sin_port, buf, len);
    return (isize)len;
  }
  if (s->type == B1NIX_SOCK_STREAM && s->tcp_conn) {
    if (!s->connected && tcp_is_established((struct tcp_conn *)s->tcp_conn)) {
      s->connected = 1;
    }
    if (!s->connected) {
      return -EAGAIN;
    }
    return tcp_send((struct tcp_conn *)s->tcp_conn, buf, len);
  }
  return -ENOTCONN;
}

isize vfs_socket_recv_h(struct vfs_handle *h, void *buf, usize len, int flags) {
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;

  /* After shutdown(SHUT_RD) the read half is closed: report EOF. */
  if (s->shut_rd)
    return 0;

  if (s->domain == B1NIX_AF_UNIX) {
    return unix_recv(s, buf, len);
  }

  if (s->type == B1NIX_SOCK_DGRAM || s->type == B1NIX_SOCK_RAW ||
      s->domain == B1NIX_AF_NETLINK) {
    while (s->udp_q_count == 0) {
      if (h->flags & B1NIX_O_NONBLOCK)
        return -EAGAIN;
      /* SMP-safe wait — see the TCP recv path below. */
      scheduler_wait_prepare(s);
      if (s->udp_q_count != 0) {
        scheduler_wait_cancel();
        break;
      }
      if (scheduler_signal_pending()) {
        scheduler_wait_cancel();
        return -ERESTARTSYS;
      }
      scheduler_wait_commit();
    }

    u8 slot = s->udp_q_head;
    usize pkt_len = s->udp_q_len[slot];
    usize to_copy = len < pkt_len ? len : pkt_len;
    memcpy(buf, s->udp_q_buf[slot], to_copy);
    if (!(flags & B1NIX_MSG_PEEK)) {
      s->udp_q_head = (u8)((s->udp_q_head + 1) % 8);
      s->udp_q_count--;
      s->recv_len = (s->udp_q_count > 0) ? s->udp_q_len[s->udp_q_head] : 0;
    }
    return (isize)to_copy;
  }
  if (s->type == B1NIX_SOCK_STREAM && s->tcp_conn) {
    struct tcp_conn *conn = (struct tcp_conn *)s->tcp_conn;
    if (!s->connected && tcp_is_established(conn)) {
      s->connected = 1;
    }
    if (!s->connected) {
      return -EAGAIN;
    }
    while (!tcp_is_readable(conn)) {
      if (h->flags & B1NIX_O_NONBLOCK) {
        return -EAGAIN;
      }
      /* SMP-safe wait: publish BLOCKED, then re-test so a wake_all(vfs_poll_chan)
       * racing in from tcp_input on another CPU can't be lost. */
      scheduler_wait_prepare(vfs_poll_chan);
      if (tcp_is_readable(conn)) {
        scheduler_wait_cancel();
        break;
      }
      if (scheduler_signal_pending()) {
        scheduler_wait_cancel();
        return -ERESTARTSYS;
      }
      scheduler_wait_commit();
    }
    return tcp_recv(conn, buf, len, flags);
  }
  return -ENOTCONN;
}

static isize socket_read(struct vfs_handle *h, char *buf, usize size) {
  return vfs_socket_recv_h(h, buf, size, 0);
}

static isize socket_write(struct vfs_handle *h, const char *buf, usize size) {
  return vfs_socket_send_h(h, buf, size, 0);
}

/* sendto()/recvfrom() wrappers: temporarily override the peer address for
 * unconnected DGRAM, then call the common vfs_socket_send_h/recv_h path.
 * This is the in-kernel equivalent of the netd_proxy_sendto/recvfrom that
 * existed during the (reverted) ring-3 netd experiment. */
isize vfs_socket_sendto(int fd, const void *buf, usize len, int flags,
                        const void *addr, usize addrlen) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (!addr || !addrlen)
    return vfs_socket_send_h(h, buf, len, flags);
  if (s->domain != B1NIX_AF_INET && s->domain != B1NIX_AF_INET6)
    return vfs_socket_send_h(h, buf, len, flags);
  /* A connected stream socket ignores the address (Linux allows it). */
  if (s->type == B1NIX_SOCK_STREAM)
    return vfs_socket_send_h(h, buf, len, flags);
  if (s->shut_wr)
    return -EPIPE;

  /* Save and override the peer address with the explicit sendto() target. */
  unsigned char saved_peer[sizeof(s->peer)];
  memcpy(saved_peer, &s->peer, sizeof(s->peer));
  if (s->domain == B1NIX_AF_INET && addrlen >= sizeof(struct b1nix_sockaddr_in)) {
    memcpy(&s->peer.in, addr, sizeof(struct b1nix_sockaddr_in));
  } else if (s->domain == B1NIX_AF_INET6 && addrlen >= sizeof(struct b1nix_sockaddr_in6)) {
    memcpy(&s->peer.in6, addr, sizeof(struct b1nix_sockaddr_in6));
  } else {
    return -EINVAL;
  }
  isize rc = vfs_socket_send_h(h, buf, len, flags);
  memcpy(&s->peer, saved_peer, sizeof(s->peer));
  return rc;
}

isize vfs_socket_recvfrom(int fd, void *buf, usize len, int flags, void *addr,
                          usize *addrlen) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (!addr || !addrlen)
    return vfs_socket_recv_h(h, buf, len, flags);

  /* SOCK_RAW/AF_NETLINK served from the in-kernel queue; source is the ip hdr */
  if (s->domain != B1NIX_AF_INET && s->domain != B1NIX_AF_INET6) {
    *addrlen = 0;
    return vfs_socket_recv_h(h, buf, len, flags);
  }
  if (s->type == B1NIX_SOCK_RAW) {
    isize rc = vfs_socket_recv_h(h, buf, len, flags);
    if (rc >= 20 && *addrlen >= sizeof(struct b1nix_sockaddr_in)) {
      const u8 *ip = (const u8 *)buf;
      struct b1nix_sockaddr_in sin;
      memset(&sin, 0, sizeof(sin));
      sin.sin_family = B1NIX_AF_INET;
      memcpy(&sin.sin_addr, ip + 12, 4);
      memcpy(addr, &sin, sizeof(sin));
      *addrlen = sizeof(sin);
    } else {
      *addrlen = 0;
    }
    return rc;
  }
  if (s->type == B1NIX_SOCK_STREAM) {
    isize rc = vfs_socket_recv_h(h, buf, len, flags);
    usize n = *addrlen < sizeof(s->peer) ? *addrlen : sizeof(s->peer);
    memcpy(addr, &s->peer, n);
    *addrlen = n;
    return rc;
  }
  if (s->shut_rd) {
    *addrlen = 0;
    return 0;
  }
  isize rc = vfs_socket_recv_h(h, buf, len, flags);
  usize n = *addrlen < sizeof(s->peer) ? *addrlen : sizeof(s->peer);
  memcpy(addr, &s->peer, n);
  *addrlen = n;
  return rc;
}

static int socket_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (s->domain == B1NIX_AF_UNIX) {
    return unix_poll(s, pfd);
  }
  
  pfd->revents = 0;
  if (s->type == B1NIX_SOCK_DGRAM || s->type == B1NIX_SOCK_RAW ||
      s->domain == B1NIX_AF_NETLINK) {
    if (s->udp_q_count > 0) pfd->revents |= B1NIX_POLLIN;
    pfd->revents |= B1NIX_POLLOUT;
  } else if (s->type == B1NIX_SOCK_STREAM) {
    if (!s->connected && s->tcp_conn &&
        tcp_is_established((struct tcp_conn *)s->tcp_conn)) {
      s->connected = 1;
    }
    if (s->connected) {
      struct tcp_conn *conn = (struct tcp_conn *)s->tcp_conn;
      if (conn) {
        if (tcp_is_readable(conn)) {
          pfd->revents |= B1NIX_POLLIN;
        }
        if (tcp_is_established(conn)) {
          pfd->revents |= B1NIX_POLLOUT;
        } else if (tcp_is_close_wait(conn)) {
          pfd->revents |= B1NIX_POLLOUT;
          pfd->revents |= B1NIX_POLLHUP;
        } else {
          pfd->revents |= B1NIX_POLLHUP;
        }
      }
    } else if (s->listening) {
      u16 port = ntoh16(s->local.in.sin_port);
      if (tcp_pending_connections(port)) {
        pfd->revents |= B1NIX_POLLIN;
      }
    }
  }
  return 0;
}

/* Tear down the underlying socket: send the TCP FIN, drop UDP bindings, free
 * the shared vfs_socket_state. This MUST run only when the LAST fd referencing
 * the handle is closed (refcount -> 0), i.e. from ->release, never from a
 * per-fd ->close. dropbear's accept()+fork() server keeps the connection fd
 * open in the child while the parent close()s its copy: tearing down on the
 * parent's close would FIN the peer (client sees "Remote closed") and free the
 * state out from under the child mid-handshake. */
static int socket_teardown(struct vfs_handle *h) {
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (!s)
    return 0;
  if (s->type == B1NIX_SOCK_RAW)
    raw_sock_unregister(s);
  if ((s->domain == B1NIX_AF_INET || s->domain == B1NIX_AF_INET6) &&
      s->type == B1NIX_SOCK_DGRAM && s->bound) {
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
      if (udp_bindings[i].used && udp_bindings[i].handle == h) {
        udp_bindings[i].used = 0;
        udp_bindings[i].port = 0;
        udp_bindings[i].handle = 0;
      }
    }
  }
  if (s->domain == B1NIX_AF_UNIX) {
    unix_free_state(s);
  } else if (s->type == B1NIX_SOCK_STREAM && s->tcp_conn) {
    tcp_close((struct tcp_conn *)s->tcp_conn);
    s->tcp_conn = 0;
  }
  kfree(s);
  h->private_data = 0;
  return 0;
}

static void socket_release(struct vfs_handle *h) {
  socket_teardown(h);
}

/* No per-fd ->close: a close() on one of several dup'd/forked references must
 * not disturb the connection. Teardown happens once, in ->release, when the
 * handle refcount reaches zero (see socket_teardown). */
/* ── Interface ioctls (SIOCGIF*) for BusyBox ifconfig ──
 * b1nix models a single configured interface, "eth0", carrying the
 * DHCP-assigned IPv4 address and the active NIC's MAC. M84: the route-mutation
 * ioctls (SIOCADDRT/SIOCDELRT) really mutate the FIB — `route add -net
 * 10.0.0.0 netmask 255.0.0.0 gw ...` now changes where packets go. */
#define SIOC_ADDRT      0x890B
#define SIOC_DELRT      0x890C
#define SIOC_GIFNAME    0x8910
#define SIOC_GIFCONF    0x8912
#define SIOC_GIFFLAGS   0x8913
#define SIOC_GIFADDR    0x8915
#define SIOC_GIFBRDADDR 0x8919
#define SIOC_GIFNETMASK 0x891B
#define SIOC_GIFMETRIC  0x891D
#define SIOC_GIFMTU     0x8921
#define SIOC_GIFHWADDR  0x8927
#define SIOC_GIFINDEX   0x8933
#define SIOC_GIFTXQLEN  0x8942

#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_RUNNING   0x40
#define IFF_MULTICAST 0x1000

#define ARPHRD_ETHER 1

struct k_ifreq {
  char ifr_name[16];
  union {
    struct b1nix_sockaddr_in addr; /* 16 bytes */
    struct {
      u16 sa_family;
      u8 sa_data[14];
    } sa;
    short flags;
    int ival;
  } u;
};

struct k_ifconf {
  int len;
  char *buf; /* user pointer (natural alignment matches userspace per arch) */
};

/* Linux `struct rtentry` (SIOCADDRT/SIOCDELRT payload), laid out to match the
 * x86_64 userspace ABI byte for byte — BusyBox `route` fills this in. The pad
 * fields are part of the ABI and must stay. */
struct k_sockaddr_generic {
  u16 sa_family;
  u8 sa_data[14];
};

struct k_rtentry {
  u64 rt_pad1;
  struct k_sockaddr_generic rt_dst;
  struct k_sockaddr_generic rt_gateway;
  struct k_sockaddr_generic rt_genmask;
  u16 rt_flags;
  i16 rt_pad2;
  u64 rt_pad3;
  u8 rt_tos;
  u8 rt_class;
  i16 rt_pad4[3];
  i16 rt_metric;
  char *rt_dev;
  u64 rt_mtu;
  u64 rt_window;
  u16 rt_irtt;
};

/* Linux `struct in6_rtmsg` — the SIOCADDRT/SIOCDELRT payload on an AF_INET6
 * socket. */
struct k_in6_rtmsg {
  u8 rtmsg_dst[16];
  u8 rtmsg_src[16];
  u8 rtmsg_gateway[16];
  u32 rtmsg_type;
  u16 rtmsg_dst_len;
  u16 rtmsg_src_len;
  u32 rtmsg_metric;
  u64 rtmsg_info;
  u32 rtmsg_flags;
  int rtmsg_ifindex;
};

/* sockaddr_in payload of an rtentry field -> host-order IPv4. sa_data[2..5] is
 * sin_addr (sa_data[0..1] is sin_port). */
static u32 rtentry_addr(const struct k_sockaddr_generic *sa) {
  return ((u32)sa->sa_data[2] << 24) | ((u32)sa->sa_data[3] << 16) |
         ((u32)sa->sa_data[4] << 8) | (u32)sa->sa_data[5];
}

static u32 net_ip_as_be(struct ipv4_addr a) {
  return (u32)a.bytes[0] | ((u32)a.bytes[1] << 8) | ((u32)a.bytes[2] << 16) |
         ((u32)a.bytes[3] << 24);
}

static int socket_ioctl(struct vfs_handle *h, u64 request, void *arg) {
  if (!arg)
    return -EFAULT;

  if (request == SIOC_ADDRT || request == SIOC_DELRT) {
    /* Linux dispatches on the socket's family: an AF_INET6 socket carries a
     * struct in6_rtmsg, an AF_INET one a struct rtentry. `route -A inet6` and
     * `ip -6 route` take the former path. */
    struct vfs_socket_state *ss = (struct vfs_socket_state *)h->private_data;
    if (ss && ss->domain == B1NIX_AF_INET6) {
      struct k_in6_rtmsg r6;
      if (syscall_copyin(&r6, arg, sizeof(r6)) != 0)
        return -EFAULT;
      struct in6_addr_k dst, gw;
      memcpy(dst.bytes, r6.rtmsg_dst, 16);
      memcpy(gw.bytes, r6.rtmsg_gateway, 16);
      u8 plen = r6.rtmsg_dst_len > 128 ? 128 : (u8)r6.rtmsg_dst_len;
      if (request == SIOC_DELRT)
        return route6_del(dst, plen, gw) == 0 ? 0 : -ESRCH;
      if (!(r6.rtmsg_flags & RTF_UP))
        return -EINVAL;
      u16 metric = r6.rtmsg_metric > 0 ? (u16)(r6.rtmsg_metric - 1) : 0;
      return route6_add(dst, plen, gw, (u16)r6.rtmsg_flags, metric,
                        r6.rtmsg_ifindex) == 0
                 ? 0
                 : -ENOBUFS;
    }

    struct k_rtentry rt;
    if (syscall_copyin(&rt, arg, sizeof(rt)) != 0)
      return -EFAULT;
    if (rt.rt_dst.sa_family != B1NIX_AF_INET ||
        (rt.rt_genmask.sa_family != B1NIX_AF_INET &&
         rt.rt_genmask.sa_family != 0))
      return -EAFNOSUPPORT;

    u32 dst = rtentry_addr(&rt.rt_dst);
    u32 mask = rtentry_addr(&rt.rt_genmask);
    u32 gw = rtentry_addr(&rt.rt_gateway);
    /* RTF_HOST means the destination is a single host: the mask field is
     * unused, the prefix is /32. */
    if (rt.rt_flags & RTF_HOST)
      mask = 0xFFFFFFFFu;

    if (request == SIOC_DELRT)
      return route_del(dst, mask, gw) == 0 ? 0 : -ESRCH;

    if (!(rt.rt_flags & RTF_UP))
      return -EINVAL;
    if ((rt.rt_flags & RTF_GATEWAY) && gw == 0)
      return -EINVAL;
    u16 metric = rt.rt_metric > 0 ? (u16)(rt.rt_metric - 1) : 0;
    /* rt_dev names the output interface ("route add ... dev eth0"); an unknown
     * or absent name leaves the route unbound (index 0 = active interface). */
    int oif = 0;
    if (rt.rt_dev) {
      char devname[16];
      usize di = 0;
      /* Byte at a time: the name may sit at the end of a page, and a bulk
       * copyin of the whole buffer would fault on the unmapped next one. */
      for (; di < sizeof(devname) - 1; di++) {
        if (syscall_copyin(&devname[di], rt.rt_dev + di, 1) != 0)
          break;
        if (devname[di] == '\0')
          break;
      }
      devname[di] = '\0';
      oif = netdev_index_by_name(devname);
    }
    return route_add_oif(dst, mask, gw, rt.rt_flags, metric, oif) == 0
               ? 0
               : -ENOBUFS;
  }

  if (request == SIOC_GIFCONF) {
    struct k_ifconf ifc;
    if (syscall_copyin(&ifc, arg, sizeof(ifc)) != 0)
      return -EFAULT;
    struct k_ifreq r;
    memset(&r, 0, sizeof(r));
    int n = 0;
    if (netdev_active()) {
      strncpy(r.ifr_name, "eth0", sizeof(r.ifr_name) - 1);
      r.u.addr.sin_family = B1NIX_AF_INET;
      r.u.addr.sin_addr = net_ip_as_be(net_get_ip());
      n = 1;
    }
    if (ifc.buf && n && ifc.len >= (int)sizeof(r)) {
      if (syscall_copyout(ifc.buf, &r, sizeof(r)) != 0)
        return -EFAULT;
    }
    ifc.len = n * (int)sizeof(r);
    if (syscall_copyout(arg, &ifc, sizeof(ifc)) != 0)
      return -EFAULT;
    return 0;
  }

  /* All remaining commands take a struct ifreq. */
  struct k_ifreq r;
  if (syscall_copyin(&r, arg, sizeof(r)) != 0)
    return -EFAULT;
  if (!netdev_active())
    return -ENODEV;

  switch (request) {
  case SIOC_GIFADDR:
    memset(&r.u, 0, sizeof(r.u));
    r.u.addr.sin_family = B1NIX_AF_INET;
    r.u.addr.sin_addr = net_ip_as_be(net_get_ip());
    break;
  case SIOC_GIFNETMASK: {
    /* M84: report the lease's real mask; fall back to /24 only when DHCP
     * never supplied option 1. */
    struct ipv4_addr nm = net_get_netmask();
    if ((nm.bytes[0] | nm.bytes[1] | nm.bytes[2] | nm.bytes[3]) == 0)
      nm = (struct ipv4_addr){{255, 255, 255, 0}};
    memset(&r.u, 0, sizeof(r.u));
    r.u.addr.sin_family = B1NIX_AF_INET;
    r.u.addr.sin_addr = net_ip_as_be(nm);
    break;
  }
  case SIOC_GIFBRDADDR: {
    /* Broadcast = address | ~netmask, derived from the lease (M84) rather
     * than assuming the last octet is the host part. */
    struct ipv4_addr ip = net_get_ip();
    struct ipv4_addr nm = net_get_netmask();
    if ((nm.bytes[0] | nm.bytes[1] | nm.bytes[2] | nm.bytes[3]) == 0)
      nm = (struct ipv4_addr){{255, 255, 255, 0}};
    struct ipv4_addr bc;
    for (int i = 0; i < 4; i++)
      bc.bytes[i] = (u8)(ip.bytes[i] | (u8)~nm.bytes[i]);
    memset(&r.u, 0, sizeof(r.u));
    r.u.addr.sin_family = B1NIX_AF_INET;
    r.u.addr.sin_addr = net_ip_as_be(bc);
    break;
  }
  case SIOC_GIFFLAGS:
    r.u.flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
    break;
  case SIOC_GIFHWADDR: {
    struct mac_addr m = net_get_mac();
    memset(&r.u, 0, sizeof(r.u));
    r.u.sa.sa_family = ARPHRD_ETHER;
    memcpy(r.u.sa.sa_data, m.bytes, 6);
    break;
  }
  case SIOC_GIFMTU:
    r.u.ival = 1500;
    break;
  case SIOC_GIFINDEX:
    /* M84: the real registration index, so a route bound to "eth0" and the
     * ifindex userspace sees agree. */
    r.u.ival = netdev_index_of(netdev_active());
    if (r.u.ival == 0)
      r.u.ival = 1;
    break;
  case SIOC_GIFMETRIC:
    r.u.ival = 0;
    break;
  case SIOC_GIFTXQLEN:
    r.u.ival = 1000;
    break;
  case SIOC_GIFNAME:
    strncpy(r.ifr_name, "eth0", sizeof(r.ifr_name) - 1);
    break;
  default:
    return -ENOTTY;
  }
  if (syscall_copyout(arg, &r, sizeof(r)) != 0)
    return -EFAULT;
  return 0;
}

const struct vfs_file_ops socket_file_ops = {
  .read = socket_read, .write = socket_write, .poll = socket_poll,
  .release = socket_release, .ioctl = socket_ioctl
};

void vfs_socket_init_handle(struct vfs_handle *h, void *socket_state) {
  h->private_data = socket_state;
  h->ops = &socket_file_ops;
  h->kind = VFS_HANDLE_SOCKET;
}

/* Linux-style atomic socket-type flags OR'd into the `type` argument (match
 * userspace <sys/socket.h>: SOCK_CLOEXEC=02000000, SOCK_NONBLOCK=00004000).
 * curl, dropbear, NetSurf and Rust std all create sockets this way. */
#define SOCK_TYPE_CLOEXEC  0x80000
#define SOCK_TYPE_NONBLOCK 0x800

int vfs_socket(int domain, int type, int protocol) {
  int sock_flags = type & (SOCK_TYPE_CLOEXEC | SOCK_TYPE_NONBLOCK);
  type &= ~(SOCK_TYPE_CLOEXEC | SOCK_TYPE_NONBLOCK);
  if (domain != B1NIX_AF_INET && domain != B1NIX_AF_INET6 &&
      domain != B1NIX_AF_UNIX && domain != B1NIX_AF_NETLINK)
    return -EAFNOSUPPORT;
  if (type != B1NIX_SOCK_DGRAM && type != B1NIX_SOCK_STREAM &&
      type != B1NIX_SOCK_RAW && type != B1NIX_SOCK_SEQPACKET)
    return -ESOCKTNOSUPPORT;
  /* SOCK_SEQPACKET exists only on the local (AF_UNIX) transport. */
  if (type == B1NIX_SOCK_SEQPACKET && domain != B1NIX_AF_UNIX)
    return -EPROTONOSUPPORT;
  /* Raw sockets are IPv4 (BusyBox ping/ICMP) or netlink (BusyBox ip). */
  if (type == B1NIX_SOCK_RAW && domain != B1NIX_AF_INET &&
      domain != B1NIX_AF_NETLINK)
    return -EAFNOSUPPORT;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_SOCKET);
  if (!h) return -ENFILE;

  struct vfs_socket_state *socket = kzalloc(sizeof(*socket));
  if (!socket) { vfs_handle_release(h); return -ENOMEM; }
  socket->domain = domain;
  socket->type = type;
  socket->protocol = protocol;

  if (domain == B1NIX_AF_UNIX) {
    int res = unix_init_state(socket);
    if (res < 0) { kfree(socket); vfs_handle_release(h); return res; }
  }
  if (type == B1NIX_SOCK_RAW && domain == B1NIX_AF_INET)
    raw_sock_register(socket);
  
  vfs_socket_init_handle(h, socket);
  
  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return -EMFILE;
  }
  if (sock_flags & SOCK_TYPE_NONBLOCK)
    h->flags |= B1NIX_O_NONBLOCK;
  if (sock_flags & SOCK_TYPE_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

/* socketpair(domain, type, protocol, sv[2]). Only AF_UNIX is supported (the
 * one POSIX requires). Creates two connected sockets, installs them as fds in
 * sv. On any failure nothing is left installed and no handle leaks. The two
 * sockets tear down independently through socket_release -> unix_free_state,
 * so closing one wakes the other and releases any in-flight SCM_RIGHTS fds. */
int vfs_socketpair(int domain, int type, int protocol, int sv[2]) {
  int sock_flags = type & (SOCK_TYPE_CLOEXEC | SOCK_TYPE_NONBLOCK);
  type &= ~(SOCK_TYPE_CLOEXEC | SOCK_TYPE_NONBLOCK);
  if (domain != B1NIX_AF_UNIX)
    return -EAFNOSUPPORT;
  if (type != B1NIX_SOCK_STREAM && type != B1NIX_SOCK_DGRAM &&
      type != B1NIX_SOCK_SEQPACKET)
    return -ESOCKTNOSUPPORT;

  struct vfs_handle *ha = 0, *hb = 0;
  struct vfs_socket_state *sa = 0, *sb = 0;
  int fda = -1, fdb = -1;

  ha = alloc_raw_handle(VFS_HANDLE_SOCKET);
  if (!ha) goto fail;
  hb = alloc_raw_handle(VFS_HANDLE_SOCKET);
  if (!hb) goto fail;

  sa = kzalloc(sizeof(*sa));
  if (!sa) goto fail;
  sb = kzalloc(sizeof(*sb));
  if (!sb) goto fail;

  sa->domain = sb->domain = domain;
  sa->type = sb->type = type;
  sa->protocol = sb->protocol = protocol;

  if (unix_init_state(sa) < 0) { kfree(sa); sa = 0; goto fail; }
  if (unix_init_state(sb) < 0) { kfree(sb); sb = 0; goto fail; }

  unix_link_pair(sa, sb);

  vfs_socket_init_handle(ha, sa);
  vfs_socket_init_handle(hb, sb);
  sa = sb = 0; /* ownership transferred to the handles */

  if (sock_flags & SOCK_TYPE_NONBLOCK) {
    ha->flags |= B1NIX_O_NONBLOCK;
    hb->flags |= B1NIX_O_NONBLOCK;
  }

  fda = scheduler_fd_alloc(ha);
  if (fda < 0) goto fail;
  fdb = scheduler_fd_alloc(hb);
  if (fdb < 0) goto fail;

  if (sock_flags & SOCK_TYPE_CLOEXEC) {
    scheduler_fd_flags_set(fda, B1NIX_FD_CLOEXEC);
    scheduler_fd_flags_set(fdb, B1NIX_FD_CLOEXEC);
  }

  sv[0] = fda;
  sv[1] = fdb;
  return 0;

fail:
  /* If fds were installed, releasing them tears down the (linked) state via
   * the normal close path; otherwise release the still-private handles/states
   * directly. */
  if (fda >= 0) {
    vfs_close(fda);
    ha = 0;
  }
  if (sa) { unix_free_state(sa); kfree(sa); }
  if (sb) { unix_free_state(sb); kfree(sb); }
  if (ha) vfs_handle_release(ha);
  if (hb) vfs_handle_release(hb);
  return -EMFILE;
}

int vfs_bind(int fd, const void *addr, usize addrlen) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;

  if (s->domain == B1NIX_AF_NETLINK) {
    /* Record AF_NETLINK at offset 0 so a later getsockname() reports the
     * nl_family BusyBox libnetlink checks. */
    s->local.in.sin_family = B1NIX_AF_NETLINK;
    s->bound = 1;
    return 0;
  }

  if (s->domain == B1NIX_AF_UNIX) {
    if (!addr || addrlen < sizeof(u16) + 1) return -EINVAL;
    return unix_bind(s, (const struct b1nix_sockaddr_un *)addr);
  }

  if (s->domain == B1NIX_AF_INET6) {
    if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in6)) return -EINVAL;
    s->local.in6 = *(const struct b1nix_sockaddr_in6 *)addr;
    s->bound = 1;
    if (s->type == B1NIX_SOCK_DGRAM) {
      u16 port = s->local.in6.sin6_port;
      if (!s->so_reuseaddr) {
        for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
          if (udp_bindings[i].used && udp_bindings[i].port == port)
            return -EADDRINUSE;
        }
      }
      for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (!udp_bindings[i].used) {
          udp_bindings[i].used = 1;
          udp_bindings[i].port = port;
          udp_bindings[i].handle = h;
          return 0;
        }
      }
      return -ENOBUFS;
    }
    return 0;
  }

  if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in)) return -EINVAL;
  s->local.in = *(const struct b1nix_sockaddr_in *)addr;
  s->bound = 1;
  if (s->type == B1NIX_SOCK_DGRAM) {
    u16 port = s->local.in.sin_port;
    if (!s->so_reuseaddr) {
      for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (udp_bindings[i].used && udp_bindings[i].port == port) {
          return -EADDRINUSE;
        }
      }
    }
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
      if (!udp_bindings[i].used) {
        udp_bindings[i].used = 1;
        udp_bindings[i].port = port;
        udp_bindings[i].handle = h;
        return 0;
      }
    }
    return -ENOBUFS;
  }
  return 0;
}

int vfs_listen(int fd, int backlog) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  s->backlog = backlog < 0 ? 0 : backlog;
  if (s->domain == B1NIX_AF_UNIX) return unix_listen(s, backlog);

  /* Re-listening on an already-listening socket is a no-op (Linux returns 0)
   * and must NOT claim a second pool slot. */
  if (s->listening) return 0;

  if (s->domain == B1NIX_AF_INET && s->type == B1NIX_SOCK_STREAM) {
    u16 port = ntoh16(s->local.in.sin_port);
    struct tcp_conn *conn = tcp_listen(port, backlog);
    if (conn) {
      s->listening = 1;
      s->tcp_conn = conn;
    }
    return conn ? 0 : -1;
  }

  if (s->domain == B1NIX_AF_INET6 && s->type == B1NIX_SOCK_STREAM) {
    u16 port = ntoh16(s->local.in6.sin6_port);
    struct tcp_conn *conn = tcp_listen(port, backlog);
    if (conn) {
      s->listening = 1;
      s->tcp_conn = conn;
    }
    return conn ? 0 : -1;
  }

  return -ENOPROTOOPT;
}

int vfs_accept(int fd, void *addr, usize *addrlen) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (!s->listening) return -EINVAL;

  struct vfs_handle *new_vh = alloc_raw_handle(VFS_HANDLE_SOCKET);
  if (!new_vh) return -ENFILE;
  struct vfs_socket_state *new_s = kzalloc(sizeof(*new_s));
  if (!new_s) { vfs_handle_release(new_vh); return -ENOMEM; }
  new_s->domain = s->domain;
  new_s->type = s->type;
  /* The accepted socket's local address is the listener's bound address. Copy
   * it so getsockname() on the connection returns a valid family/addr (sshd
   * calls getsockname right after accept; a zeroed family fails getnameinfo). */
  new_s->local = s->local;

  int res = 0;
  if (s->domain == B1NIX_AF_UNIX) {
    unix_init_state(new_s);
    res = unix_accept(s, new_s, (h->flags & B1NIX_O_NONBLOCK) != 0);
    if (res == 0 && addr && addrlen && *addrlen >= sizeof(struct b1nix_sockaddr_un)) {
      memcpy(addr, &new_s->peer.un, sizeof(struct b1nix_sockaddr_un));
      *addrlen = sizeof(struct b1nix_sockaddr_un);
    }
  } else if (s->domain == B1NIX_AF_INET && s->type == B1NIX_SOCK_STREAM) {
    u16 local_port = ntoh16(s->local.in.sin_port);
    struct ipv4_addr client_ip;
    u16 client_port;
    struct tcp_conn *conn = 0;
    while (1) {
      conn = tcp_accept(local_port, &client_ip, &client_port);
      if (conn) break;
      if (h->flags & B1NIX_O_NONBLOCK) {
        kfree(new_s);
        vfs_handle_release(new_vh);
        return -EAGAIN;
      }
      /* SMP-safe wait: publish BLOCKED, then re-poll tcp_accept so a connection
       * that lands (and its wake_all(vfs_poll_chan)) between the test and the
       * block isn't lost. */
      scheduler_wait_prepare(vfs_poll_chan);
      conn = tcp_accept(local_port, &client_ip, &client_port);
      if (conn) {
        scheduler_wait_cancel();
        break;
      }
      if (scheduler_signal_pending()) {
        scheduler_wait_cancel();
        kfree(new_s);
        vfs_handle_release(new_vh);
        return -ERESTARTSYS;
      }
      scheduler_wait_commit();
    }
    new_s->tcp_conn = conn;
    new_s->connected = 1;
    new_s->peer.in.sin_family = B1NIX_AF_INET;
    new_s->peer.in.sin_port = (client_port << 8) | (client_port >> 8);
    new_s->peer.in.sin_addr = (u32)client_ip.bytes[0] | ((u32)client_ip.bytes[1] << 8) |
                              ((u32)client_ip.bytes[2] << 16) | ((u32)client_ip.bytes[3] << 24);
    
    if (addr && addrlen && *addrlen >= sizeof(struct b1nix_sockaddr_in)) {
      memcpy(addr, &new_s->peer.in, sizeof(struct b1nix_sockaddr_in));
      *addrlen = sizeof(struct b1nix_sockaddr_in);
    }
    res = 0;
  } else if (s->domain == B1NIX_AF_INET6 && s->type == B1NIX_SOCK_STREAM) {
    u16 local_port = ntoh16(s->local.in6.sin6_port);
    struct in6_addr_k client_ip6;
    u16 client_port;
    struct tcp_conn *conn = 0;
    while (1) {
      conn = tcp_accept6(local_port, &client_ip6, &client_port);
      if (conn) break;
      if (h->flags & B1NIX_O_NONBLOCK) {
        kfree(new_s);
        vfs_handle_release(new_vh);
        return -EAGAIN;
      }
      /* SMP-safe wait — see the IPv4 accept path above. */
      scheduler_wait_prepare(vfs_poll_chan);
      conn = tcp_accept6(local_port, &client_ip6, &client_port);
      if (conn) {
        scheduler_wait_cancel();
        break;
      }
      if (scheduler_signal_pending()) {
        scheduler_wait_cancel();
        kfree(new_s);
        vfs_handle_release(new_vh);
        return -ERESTARTSYS;
      }
      scheduler_wait_commit();
    }
    new_s->tcp_conn = conn;
    new_s->connected = 1;
    new_s->peer.in6.sin6_family = B1NIX_AF_INET6;
    new_s->peer.in6.sin6_port = (client_port << 8) | (client_port >> 8);
    memcpy(new_s->peer.in6.sin6_addr.s6_addr, client_ip6.bytes, 16);

    if (addr && addrlen && *addrlen >= sizeof(struct b1nix_sockaddr_in6)) {
      memcpy(addr, &new_s->peer.in6, sizeof(struct b1nix_sockaddr_in6));
      *addrlen = sizeof(struct b1nix_sockaddr_in6);
    }
    res = 0;
  } else {
    kfree(new_s);
    vfs_handle_release(new_vh);
    return -ENOPROTOOPT;
  }
  
  if (res < 0) { 
    if (s->domain == B1NIX_AF_UNIX) unix_free_state(new_s);
    kfree(new_s); 
    vfs_handle_release(new_vh); 
    return res; 
  }

  vfs_socket_init_handle(new_vh, new_s);
  
  int new_fd = scheduler_fd_alloc(new_vh);
  if (new_fd < 0) {
    vfs_handle_release(new_vh);
    return -EMFILE;
  }
  return new_fd;
}

int vfs_connect(int fd, const void *addr, usize addrlen) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (s->connected) return -EISCONN;
  if (s->tcp_conn && !s->connected) {
    if (tcp_is_established((struct tcp_conn *)s->tcp_conn)) {
      s->connected = 1;
      return 0;
    }
    return -EALREADY;
  }
  
  if (s->domain == B1NIX_AF_UNIX) {
    if (!addr || addrlen < sizeof(u16) + 1) return -EINVAL;
    return unix_connect(s, (const struct b1nix_sockaddr_un *)addr);
  }

  if (s->domain == B1NIX_AF_INET6) {
    if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in6)) return -EINVAL;
    s->peer.in6 = *(const struct b1nix_sockaddr_in6 *)addr;
    s->connected = 0;
    struct in6_addr_k dst;
    memcpy(dst.bytes, s->peer.in6.sin6_addr.s6_addr, 16);
    if (s->ipv6_v6only && in6_is_v4mapped(&dst))
      return -EAFNOSUPPORT;
    if (s->type == B1NIX_SOCK_STREAM) {
      s->tcp_conn = tcp_connect6(dst, ntoh16(s->peer.in6.sin6_port));
      if (!s->tcp_conn)
        return -ECONNREFUSED;
      s->connected = 1;
    }
    /* Datagram connect just records the default peer for subsequent send()s. */
    return 0;
  }

  if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in)) return -EINVAL;
  s->peer.in = *(const struct b1nix_sockaddr_in *)addr;
  s->connected = 0;
  if (s->type == B1NIX_SOCK_STREAM) {
    struct ipv4_addr dst_ip;
    dst_ip.bytes[0] = s->peer.in.sin_addr & 0xFF;
    dst_ip.bytes[1] = (s->peer.in.sin_addr >> 8) & 0xFF;
    dst_ip.bytes[2] = (s->peer.in.sin_addr >> 16) & 0xFF;
    dst_ip.bytes[3] = (s->peer.in.sin_addr >> 24) & 0xFF;
    if (h->flags & B1NIX_O_NONBLOCK) {
      s->tcp_conn = tcp_connect_async(dst_ip, ntoh16(s->peer.in.sin_port));
      if (!s->tcp_conn) {
        return -ECONNREFUSED;
      }
      return -EINPROGRESS;
    }
    s->tcp_conn = tcp_connect(dst_ip, ntoh16(s->peer.in.sin_port));
    if (!s->tcp_conn) {
      s->connected = 0;
      return -ECONNREFUSED;
    }
    s->connected = 1;
  }
  return 0;
}

isize vfs_socket_send(int fd, const void *buf, usize len, int flags) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  return vfs_socket_send_h(h, buf, len, flags);
}

isize vfs_socket_recv(int fd, void *buf, usize len, int flags) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  return vfs_socket_recv_h(h, buf, len, flags);
}

isize vfs_socket_sendmsg(int fd, const void *buf, usize len, int flags,
                         struct vfs_handle **handles, usize nhandles,
                         const struct b1nix_ucred *cred) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h)
    return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET)
    return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (nhandles || cred) {
    if (s->domain != B1NIX_AF_UNIX)
      return -EOPNOTSUPP;
    if (nhandles > VFS_SCM_MAX_FDS)
      return -EINVAL;
    return unix_send_control(s, buf, len, handles, nhandles, cred,
                             (h->flags & B1NIX_O_NONBLOCK) ||
                                 (flags & B1NIX_MSG_DONTWAIT));
  }
  return vfs_socket_send_h(h, buf, len, flags);
}

isize vfs_socket_recvmsg(int fd, void *buf, usize len, int flags,
                         int *received_fds, usize fd_capacity,
                         usize *received_count, struct b1nix_ucred *cred,
                         int *has_cred, int *control_truncated) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h)
    return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET)
    return -ENOTSOCK;
  if (received_count)
    *received_count = 0;
  if (has_cred)
    *has_cred = 0;
  if (control_truncated)
    *control_truncated = 0;

  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (s->domain != B1NIX_AF_UNIX)
    return vfs_socket_recv_h(h, buf, len, flags);

  struct vfs_handle *handles[VFS_SCM_MAX_FDS] = {0};
  usize nhandles = 0;
  /* Honor the fd's O_NONBLOCK: unix_recv_control only blocks unless
   * MSG_DONTWAIT is set, so fold the handle's non-blocking flag in. Without
   * this a recvmsg(flags=0) on an O_NONBLOCK unix socket blocks on an idle
   * peer — e.g. displayd's per-client drain wedges when one client goes quiet. */
  int recv_flags = flags;
  if (h->flags & B1NIX_O_NONBLOCK)
    recv_flags |= B1NIX_MSG_DONTWAIT;
  isize rc = unix_recv_control(s, buf, len, recv_flags, handles, &nhandles, cred,
                               has_cred);
  if (rc < 0 || (recv_flags & B1NIX_MSG_PEEK))
    return rc;

  usize installed = 0;
  for (usize i = 0; i < nhandles; i++) {
    if (installed < fd_capacity) {
      int newfd = scheduler_fd_alloc(handles[i]);
      if (newfd >= 0) {
        received_fds[installed++] = newfd;
        handles[i] = 0;
        continue;
      }
    }
    if (handles[i]) {
      vfs_handle_release(handles[i]);
      handles[i] = 0;
    }
    if (control_truncated)
      *control_truncated = 1;
  }
  if (received_count)
    *received_count = installed;
  return rc;
}

/* ---- M32b: socket option / address / shutdown API ----
 * Option name/level values match userspace <sys/socket.h>/<netinet/tcp.h>
 * (Linux-compatible numbering). */
#define SOCK_SOL_SOCKET   1
#define SOCK_IPPROTO_TCP  6
#define SOCK_IPPROTO_IPV6 41
#define SOCK_SO_REUSEADDR 2
#define SOCK_SO_TYPE      3
#define SOCK_SO_ERROR     4
#define SOCK_SO_SNDBUF    7
#define SOCK_SO_RCVBUF    8
#define SOCK_SO_KEEPALIVE 9
#define SOCK_SO_REUSEPORT 15
#define SOCK_SO_PASSCRED  16
#define SOCK_SO_PEERCRED  17
#define SOCK_SO_ACCEPTCONN 30
#define SOCK_TCP_NODELAY  1
#define SOCK_IPV6_V6ONLY  26
#define SOCK_SHUT_RD      0
#define SOCK_SHUT_WR      1
#define SOCK_SHUT_RDWR    2

static struct vfs_socket_state *socket_state_for_fd(int fd, int *err) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) { *err = -EBADF; return 0; }
  if (h->kind != VFS_HANDLE_SOCKET) { *err = -ENOTSOCK; return 0; }
  *err = 0;
  return (struct vfs_socket_state *)h->private_data;
}

int vfs_setsockopt(int fd, int level, int optname, const void *optval,
                   usize optlen) {
  int err;
  struct vfs_socket_state *s = socket_state_for_fd(fd, &err);
  if (!s) return err;
  if (!optval || optlen < sizeof(int)) return -EINVAL;
  int v = *(const int *)optval;

  if (level == SOCK_SOL_SOCKET) {
    switch (optname) {
    case SOCK_SO_REUSEADDR:
    case SOCK_SO_REUSEPORT: s->so_reuseaddr = v ? 1 : 0; return 0;
    case SOCK_SO_KEEPALIVE: s->so_keepalive = v ? 1 : 0; return 0;
    case SOCK_SO_SNDBUF:    s->so_sndbuf = v; return 0;
    case SOCK_SO_RCVBUF:    s->so_rcvbuf = v; return 0;
    case SOCK_SO_PASSCRED:
      if (s->domain != B1NIX_AF_UNIX) return -ENOPROTOOPT;
      s->so_passcred = v ? 1 : 0;
      return 0;
    case SOCK_SO_ERROR:     return -ENOPROTOOPT; /* read-only */
    default:                return -ENOPROTOOPT;
    }
  }
  if (level == SOCK_IPPROTO_TCP) {
    if (optname == SOCK_TCP_NODELAY) {
      /* b1nix TCP already sends each segment promptly (no Nagle), so this is
       * a stored, honoured-by-construction flag. */
      s->tcp_nodelay = v ? 1 : 0;
      return 0;
    }
    return -ENOPROTOOPT;
  }
  if (level == SOCK_IPPROTO_IPV6 && optname == SOCK_IPV6_V6ONLY) {
    if (s->domain != B1NIX_AF_INET6)
      return -ENOPROTOOPT;
    if (s->bound || s->connected || s->listening)
      return -EINVAL;
    s->ipv6_v6only = v ? 1 : 0;
    return 0;
  }
  return -ENOPROTOOPT;
}

int vfs_getsockopt(int fd, int level, int optname, void *optval,
                   usize *optlen) {
  int err;
  struct vfs_socket_state *s = socket_state_for_fd(fd, &err);
  if (!s) return err;
  if (!optval || !optlen) return -EINVAL;

  /* SO_PEERCRED is the one option whose value is not an int: it returns a
   * struct ucred {pid,uid,gid}. Handle it before the int-sized checks below. */
  if (level == SOCK_SOL_SOCKET && optname == SOCK_SO_PEERCRED) {
    if (s->domain != B1NIX_AF_UNIX) return -ENOPROTOOPT;
    struct b1nix_ucred cred;
    int rc = unix_peer_cred(s, &cred);
    if (rc < 0) return rc;
    usize need = sizeof(cred);
    if (*optlen < need) return -EINVAL;
    memcpy(optval, &cred, need);
    *optlen = need;
    return 0;
  }

  if (*optlen < sizeof(int)) return -EINVAL;
  int v = 0;

  if (level == SOCK_SOL_SOCKET) {
    switch (optname) {
    case SOCK_SO_REUSEADDR:
    case SOCK_SO_REUSEPORT: v = s->so_reuseaddr; break;
    case SOCK_SO_KEEPALIVE: v = s->so_keepalive; break;
    case SOCK_SO_TYPE:      v = s->type; break;
    case SOCK_SO_ERROR:     v = s->so_error; s->so_error = 0; break;
    case SOCK_SO_SNDBUF:    v = s->so_sndbuf; break;
    case SOCK_SO_RCVBUF:    v = s->so_rcvbuf; break;
    case SOCK_SO_ACCEPTCONN: v = s->listening; break;
    case SOCK_SO_PASSCRED:  v = s->so_passcred; break;
    default:                return -ENOPROTOOPT;
    }
  } else if (level == SOCK_IPPROTO_TCP && optname == SOCK_TCP_NODELAY) {
    v = s->tcp_nodelay;
  } else if (level == SOCK_IPPROTO_IPV6 && optname == SOCK_IPV6_V6ONLY &&
             s->domain == B1NIX_AF_INET6) {
    v = s->ipv6_v6only;
  } else {
    return -ENOPROTOOPT;
  }

  *(int *)optval = v;
  *optlen = sizeof(int);
  return 0;
}

static int sock_copy_local_peer(struct vfs_socket_state *s, int want_peer,
                                void *addr, usize *addrlen) {
  if (!addr || !addrlen) return -EINVAL;
  union {
    struct b1nix_sockaddr_in in;
    struct b1nix_sockaddr_in6 in6;
    struct b1nix_sockaddr_un un;
  } *src = want_peer ? (void *)&s->peer : (void *)&s->local;
  usize need;
  if (s->domain == B1NIX_AF_INET) need = sizeof(struct b1nix_sockaddr_in);
  else if (s->domain == B1NIX_AF_INET6) need = sizeof(struct b1nix_sockaddr_in6);
  else need = sizeof(struct b1nix_sockaddr_un);

  usize copy = *addrlen < need ? *addrlen : need;
  memcpy(addr, src, copy);
  *addrlen = need; /* report the full (untruncated) length, like Linux */
  return 0;
}

int vfs_getsockname(int fd, void *addr, usize *addrlen) {
  int err;
  struct vfs_socket_state *s = socket_state_for_fd(fd, &err);
  if (!s) return err;
  return sock_copy_local_peer(s, 0, addr, addrlen);
}

int vfs_getpeername(int fd, void *addr, usize *addrlen) {
  int err;
  struct vfs_socket_state *s = socket_state_for_fd(fd, &err);
  if (!s) return err;
  if (!s->connected) return -ENOTCONN;
  return sock_copy_local_peer(s, 1, addr, addrlen);
}

int vfs_shutdown(int fd, int how) {
  int err;
  struct vfs_socket_state *s = socket_state_for_fd(fd, &err);
  if (!s) return err;
  if (how != SOCK_SHUT_RD && how != SOCK_SHUT_WR && how != SOCK_SHUT_RDWR)
    return -EINVAL;
  if (how == SOCK_SHUT_RD || how == SOCK_SHUT_RDWR) s->shut_rd = 1;
  if (how == SOCK_SHUT_WR || how == SOCK_SHUT_RDWR) s->shut_wr = 1;
  /* Wake any blocked reader so it observes the now-closed half. */
  scheduler_wake_all(s);
  scheduler_wake_all(vfs_poll_chan);
  return 0;
}

int vfs_socket_push_udp(u16 local_port_net, const void *data, usize len) {
  for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
    if (udp_bindings[i].used && udp_bindings[i].port == local_port_net) {
      struct vfs_handle *h = udp_bindings[i].handle;
      if (!h || !h->used || h->kind != VFS_HANDLE_SOCKET) {
        udp_bindings[i].used = 0;
        continue;
      }
      struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
      if (s->udp_q_count >= 8) {
        return 0;
      }
      u8 slot = s->udp_q_tail;
      usize copy = (len > sizeof(s->udp_q_buf[slot])) ? sizeof(s->udp_q_buf[slot]) : len;
      memcpy(s->udp_q_buf[slot], data, copy);
      s->udp_q_len[slot] = copy;
      s->udp_q_tail = (u8)((s->udp_q_tail + 1) % 8);
      s->udp_q_count++;
      s->recv_len = s->udp_q_len[s->udp_q_head];
      
      /* Wake up polling tasks */
      scheduler_wake_all(s);
      scheduler_wake_all(vfs_poll_chan);
      
      return 1;
    }
  }
  return 0;
}

usize udp_binding_snapshot(struct net_sock_info *out, usize max) {
  usize n = 0;
  for (int i = 0; i < MAX_UDP_BINDINGS && n < max; i++) {
    if (!udp_bindings[i].used)
      continue;
    struct net_sock_info *e = &out[n++];
    memset(e, 0, sizeof(*e));
    e->family = 4;
    e->local_port = udp_bindings[i].port;
    e->state = 0x07; /* Linux marks UDP sockets CLOSE(7); netstat ignores it */
  }
  return n;
}
