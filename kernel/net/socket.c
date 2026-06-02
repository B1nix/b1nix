#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <stdlib.h>
#include <string.h>
#include <b1nix/net.h>

struct udp_binding {
  int used;
  u16 port;
  struct vfs_handle *handle;
};
#define MAX_UDP_BINDINGS 64
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
isize vfs_socket_send_h(struct vfs_handle *h, const void *buf, usize len, int flags) {
  (void)flags;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;

  /* After shutdown(SHUT_WR) the write half is closed: POSIX requires EPIPE. */
  if (s->shut_wr)
    return -EPIPE;

  if (s->domain == B1NIX_AF_UNIX) {
    return unix_send(s, buf, len);
  }

  if (s->domain == B1NIX_AF_INET6) {
    if (s->type == B1NIX_SOCK_DGRAM) {
      if (!s->connected && s->peer.in6.sin6_port == 0)
        return -ENOTCONN;
      struct in6_addr_k dst;
      memcpy(dst.bytes, s->peer.in6.sin6_addr.s6_addr, 16);
      if (in6_is_v4mapped(&dst)) {
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

  if (s->type == B1NIX_SOCK_DGRAM) {
    while (s->udp_q_count == 0) {
      if (h->flags & B1NIX_O_NONBLOCK)
        return -EAGAIN;
      /* SMP-safe wait — see the TCP recv path below. */
      scheduler_wait_prepare(s);
      if (s->udp_q_count != 0) {
        scheduler_wait_cancel();
        break;
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

static int socket_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (s->domain == B1NIX_AF_UNIX) {
    return unix_poll(s, pfd);
  }
  
  pfd->revents = 0;
  if (s->type == B1NIX_SOCK_DGRAM) {
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
const struct vfs_file_ops socket_file_ops = {
  .read = socket_read, .write = socket_write, .poll = socket_poll, .release = socket_release
};

void vfs_socket_init_handle(struct vfs_handle *h, void *socket_state) {
  h->private_data = socket_state;
  h->ops = &socket_file_ops;
  h->kind = VFS_HANDLE_SOCKET;
}

int vfs_socket(int domain, int type, int protocol) {
  if (domain != B1NIX_AF_INET && domain != B1NIX_AF_INET6 &&
      domain != B1NIX_AF_UNIX) return -EAFNOSUPPORT;
  if (type != B1NIX_SOCK_DGRAM && type != B1NIX_SOCK_STREAM) return -ESOCKTNOSUPPORT;
  
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
  
  vfs_socket_init_handle(h, socket);
  
  int fd = scheduler_fd_alloc(h);
  if (fd < 0) { 
    vfs_handle_release(h); 
    return -EMFILE; 
  }
  return fd;
}

int vfs_bind(int fd, const void *addr, usize addrlen) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  
  if (s->domain == B1NIX_AF_UNIX) {
    if (addrlen < sizeof(struct b1nix_sockaddr_un)) return -EINVAL;
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
  
  if (s->domain == B1NIX_AF_INET && s->type == B1NIX_SOCK_STREAM) {
    u16 port = ntoh16(s->local.in.sin_port);
    int res = tcp_listen(port, backlog);
    if (res == 0) s->listening = 1;
    return res;
  }

  if (s->domain == B1NIX_AF_INET6 && s->type == B1NIX_SOCK_STREAM) {
    u16 port = ntoh16(s->local.in6.sin6_port);
    int res = tcp_listen(port, backlog);
    if (res == 0) s->listening = 1;
    return res;
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
    res = unix_accept(s, new_s);
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
    if (addrlen < sizeof(struct b1nix_sockaddr_un)) return -EINVAL;
    return unix_connect(s, (const struct b1nix_sockaddr_un *)addr);
  }

  if (s->domain == B1NIX_AF_INET6) {
    if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in6)) return -EINVAL;
    s->peer.in6 = *(const struct b1nix_sockaddr_in6 *)addr;
    s->connected = 0;
    if (s->type == B1NIX_SOCK_STREAM) {
      struct in6_addr_k dst;
      memcpy(dst.bytes, s->peer.in6.sin6_addr.s6_addr, 16);
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

/* ---- M32b: socket option / address / shutdown API ----
 * Option name/level values match userspace <sys/socket.h>/<netinet/tcp.h>
 * (Linux-compatible numbering). */
#define SOCK_SOL_SOCKET   1
#define SOCK_IPPROTO_TCP  6
#define SOCK_SO_REUSEADDR 2
#define SOCK_SO_TYPE      3
#define SOCK_SO_ERROR     4
#define SOCK_SO_SNDBUF    7
#define SOCK_SO_RCVBUF    8
#define SOCK_SO_KEEPALIVE 9
#define SOCK_SO_REUSEPORT 15
#define SOCK_SO_ACCEPTCONN 30
#define SOCK_TCP_NODELAY  1
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
  return -ENOPROTOOPT;
}

int vfs_getsockopt(int fd, int level, int optname, void *optval,
                   usize *optlen) {
  int err;
  struct vfs_socket_state *s = socket_state_for_fd(fd, &err);
  if (!s) return err;
  if (!optval || !optlen || *optlen < sizeof(int)) return -EINVAL;
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
    default:                return -ENOPROTOOPT;
    }
  } else if (level == SOCK_IPPROTO_TCP && optname == SOCK_TCP_NODELAY) {
    v = s->tcp_nodelay;
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
