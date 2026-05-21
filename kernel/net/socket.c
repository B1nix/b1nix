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
  int h_idx;
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
  (void)flags;
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
    s->udp_q_head = (u8)((s->udp_q_head + 1) % 8);
    s->udp_q_count--;
    s->recv_len = (s->udp_q_count > 0) ? s->udp_q_len[s->udp_q_head] : 0;
    return (isize)to_copy;
  }
  if (s->type == B1NIX_SOCK_STREAM && s->tcp_conn) {
    if (!s->connected && tcp_is_established((struct tcp_conn *)s->tcp_conn)) {
      s->connected = 1;
    }
    if (!s->connected) {
      return -EAGAIN;
    }
    return tcp_recv((struct tcp_conn *)s->tcp_conn, buf, len);
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
      pfd->revents |= B1NIX_POLLIN | B1NIX_POLLOUT;
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
  if (s->domain == B1NIX_AF_UNIX) {
    unix_free_state(s);
  } else if (s->type == B1NIX_SOCK_STREAM && s->tcp_conn) {
    tcp_close((struct tcp_conn *)s->tcp_conn);
    s->tcp_conn = 0;
  }
  kfree(s);
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
  if (domain != B1NIX_AF_INET && domain != B1NIX_AF_UNIX) return -EAFNOSUPPORT;
  if (type != B1NIX_SOCK_DGRAM && type != B1NIX_SOCK_STREAM) return -ESOCKTNOSUPPORT;
  
  int handle_idx = alloc_raw_handle(VFS_HANDLE_SOCKET);
  if (handle_idx < 0) return handle_idx;
  
  struct vfs_socket_state *socket = kzalloc(sizeof(*socket));
  if (!socket) { release_handle(handle_idx); return -ENOMEM; }
  socket->domain = domain;
  socket->type = type;
  socket->protocol = protocol;
  
  if (domain == B1NIX_AF_UNIX) {
    int res = unix_init_state(socket);
    if (res < 0) { kfree(socket); release_handle(handle_idx); return res; }
  }
  
  struct vfs_handle *h = get_handle_by_idx(handle_idx);
  vfs_socket_init_handle(h, socket);
  
  int fd = scheduler_fd_alloc(handle_idx);
  if (fd < 0) { 
    if (domain == B1NIX_AF_UNIX) unix_free_state(socket);
    kfree(socket); 
    release_handle(handle_idx); 
    return -EMFILE; 
  }
  return fd;
}

int vfs_bind(int fd, const void *addr, usize addrlen) {
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) return -EBADF;
  struct vfs_handle *h = get_handle_by_idx(h_idx);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  
  if (s->domain == B1NIX_AF_UNIX) {
    if (addrlen < sizeof(struct b1nix_sockaddr_un)) return -EINVAL;
    return unix_bind(s, (const struct b1nix_sockaddr_un *)addr);
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
        udp_bindings[i].h_idx = h_idx;
        return 0;
      }
    }
    return -ENOBUFS;
  }
  return 0;
}

int vfs_listen(int fd, int backlog) {
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) return -EBADF;
  struct vfs_handle *h = get_handle_by_idx(h_idx);
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
  
  return -ENOPROTOOPT;
}

int vfs_accept(int fd, void *addr, usize *addrlen) {
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) return -EBADF;
  struct vfs_handle *h = get_handle_by_idx(h_idx);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (!s->listening) return -EINVAL;

  int new_h_idx = alloc_raw_handle(VFS_HANDLE_SOCKET);
  if (new_h_idx < 0) return new_h_idx;
  struct vfs_socket_state *new_s = kzalloc(sizeof(*new_s));
  if (!new_s) { release_handle(new_h_idx); return -ENOMEM; }
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
        release_handle(new_h_idx);
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
  } else {
    kfree(new_s);
    release_handle(new_h_idx);
    return -ENOPROTOOPT;
  }
  
  if (res < 0) { 
    if (s->domain == B1NIX_AF_UNIX) unix_free_state(new_s);
    kfree(new_s); 
    release_handle(new_h_idx); 
    return res; 
  }

  struct vfs_handle *new_vh = get_handle_by_idx(new_h_idx);
  vfs_socket_init_handle(new_vh, new_s);
  
  return scheduler_fd_alloc(new_h_idx);
}

int vfs_connect(int fd, const void *addr, usize addrlen) {
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) return -EBADF;
  struct vfs_handle *h = get_handle_by_idx(h_idx);
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
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) return -EBADF;
  struct vfs_handle *h = get_handle_by_idx(h_idx);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  return vfs_socket_send_h(h, buf, len, flags);
}

isize vfs_socket_recv(int fd, void *buf, usize len, int flags) {
  int h_idx = scheduler_fd_get(fd);
  if (h_idx < 0) return -EBADF;
  struct vfs_handle *h = get_handle_by_idx(h_idx);
  if (!h) return -EBADF;
  if (h->kind != VFS_HANDLE_SOCKET) return -ENOTSOCK;
  return vfs_socket_recv_h(h, buf, len, flags);
}

int vfs_socket_push_udp(u16 local_port_net, const void *data, usize len) {
  for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
    if (udp_bindings[i].used && udp_bindings[i].port == local_port_net) {
      int h_idx = udp_bindings[i].h_idx;
      struct vfs_handle *h = get_handle_by_idx(h_idx);
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
