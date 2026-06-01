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

/* Bridge functions to avoid including private net headers in vfs.h */
isize vfs_socket_send_h(struct vfs_handle *h, const void *buf, usize len, int flags) {
  (void)flags;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  
  if (s->domain == B1NIX_AF_UNIX) {
    return unix_send(s, buf, len);
  }

  if (s->domain == B1NIX_AF_INET6) {
    if (s->type == B1NIX_SOCK_DGRAM) {
      if (!s->connected && s->peer.in6.sin6_port == 0)
        return -ENOTCONN;
      struct in6_addr_k dst;
      memcpy(dst.bytes, s->peer.in6.sin6_addr.s6_addr, 16);
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
  
  if (s->domain == B1NIX_AF_UNIX) {
    return unix_recv(s, buf, len);
  }

  if (s->type == B1NIX_SOCK_DGRAM) {
    while (s->udp_q_count == 0) {
      if (h->flags & B1NIX_O_NONBLOCK)
        return -EAGAIN;
      scheduler_block_on(s);
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
      scheduler_block_on(vfs_poll_chan);
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
    /* Simple poll for TCP */
    if (s->connected) {
      struct tcp_conn *conn = (struct tcp_conn *)s->tcp_conn;
      if (conn && tcp_is_readable(conn)) {
        pfd->revents |= B1NIX_POLLIN;
      }
      pfd->revents |= B1NIX_POLLOUT;
    } else if (s->listening) {
      u16 port = ntoh16(s->local.in.sin_port);
      if (tcp_pending_connections(port)) {
        pfd->revents |= B1NIX_POLLIN;
      }
    }
  }
  return 0;
}

static int socket_close(struct vfs_handle *h) {
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
  socket_close(h);
}

const struct vfs_file_ops socket_file_ops = {
  .read = socket_read, .write = socket_write, .poll = socket_poll, .close = socket_close, .release = socket_release
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
      for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
        if (udp_bindings[i].used && udp_bindings[i].port == port)
          return -EADDRINUSE;
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
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
      if (udp_bindings[i].used && udp_bindings[i].port == port) {
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

int vfs_listen(int fd, int backlog) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
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
      scheduler_block_on(vfs_poll_chan);
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
      scheduler_block_on(vfs_poll_chan);
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
