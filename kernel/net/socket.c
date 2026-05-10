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

/* Bridge functions to avoid including private net headers in vfs.h */
isize vfs_socket_send_h(struct vfs_handle *h, const void *buf, usize len, int flags) {
  (void)flags;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  struct ipv4_addr dst_ip;
  dst_ip.bytes[0] = s->peer.sin_addr & 0xFF;
  dst_ip.bytes[1] = (s->peer.sin_addr >> 8) & 0xFF;
  dst_ip.bytes[2] = (s->peer.sin_addr >> 16) & 0xFF;
  dst_ip.bytes[3] = (s->peer.sin_addr >> 24) & 0xFF;

  if (s->type == B1NIX_SOCK_DGRAM) {
    udp_send(dst_ip, (s->local.sin_port << 8) | (s->local.sin_port >> 8), (s->peer.sin_port << 8) | (s->peer.sin_port >> 8), buf, len);
    return (isize)len;
  }
  if (s->type == B1NIX_SOCK_STREAM && s->tcp_conn) {
    return tcp_send((struct tcp_conn *)s->tcp_conn, buf, len);
  }
  return -1;
}

isize vfs_socket_recv_h(struct vfs_handle *h, void *buf, usize len, int flags) {
  (void)flags;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (s->type == B1NIX_SOCK_DGRAM) {
    if (s->recv_len == 0) return -EAGAIN;
    usize to_copy = len < s->recv_len ? len : s->recv_len;
    memcpy(buf, s->recv_buf, to_copy);
    s->recv_len = 0;
    return (isize)to_copy;
  }
  if (s->type == B1NIX_SOCK_STREAM && s->tcp_conn) {
    return tcp_recv((struct tcp_conn *)s->tcp_conn, buf, len);
  }
  return -1;
}

static isize socket_read(struct vfs_handle *h, char *buf, usize size) {
  return vfs_socket_recv_h(h, buf, size, 0);
}

static isize socket_write(struct vfs_handle *h, const char *buf, usize size) {
  return vfs_socket_send_h(h, buf, size, 0);
}

static int socket_close(struct vfs_handle *h) {
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (s->type == B1NIX_SOCK_STREAM && s->tcp_conn) {
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
  .read = socket_read, .write = socket_write, .close = socket_close, .release = socket_release
};

void vfs_socket_init_handle(struct vfs_handle *h, void *socket_state) {
  h->private_data = socket_state;
  h->ops = &socket_file_ops;
  h->kind = VFS_HANDLE_SOCKET;
}

int vfs_socket(int domain, int type, int protocol) {
  if (domain != B1NIX_AF_INET) return -1;
  if (type != B1NIX_SOCK_DGRAM && type != B1NIX_SOCK_STREAM) return -1;
  
  int handle_idx = alloc_raw_handle(VFS_HANDLE_SOCKET);
  if (handle_idx < 0) return handle_idx;
  
  struct vfs_socket_state *socket = kzalloc(sizeof(*socket));
  if (!socket) { release_handle(handle_idx); return -ENOMEM; }
  socket->domain = domain;
  socket->type = type;
  socket->protocol = protocol;
  
  struct vfs_handle *h = get_handle_by_idx(handle_idx);
  vfs_socket_init_handle(h, socket);
  
  int fd = scheduler_fd_alloc(handle_idx);
  if (fd < 0) { release_handle(handle_idx); return -1; }
  return fd;
}

int vfs_bind(int fd, const void *addr, usize addrlen) {
  struct vfs_handle *h = get_handle_by_idx(scheduler_fd_get(fd));
  if (!h || h->kind != VFS_HANDLE_SOCKET) return -1;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in)) return -1;
  s->local = *(const struct b1nix_sockaddr_in *)addr;
  s->bound = 1;
  if (s->type == B1NIX_SOCK_DGRAM) {
    u16 port = (s->local.sin_port << 8) | (s->local.sin_port >> 8);
    for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
      if (!udp_bindings[i].used) {
        udp_bindings[i].used = 1;
        udp_bindings[i].port = port;
        udp_bindings[i].h_idx = scheduler_fd_get(fd);
        break;
      }
    }
  }
  return 0;
}

int vfs_connect(int fd, const void *addr, usize addrlen) {
  struct vfs_handle *h = get_handle_by_idx(scheduler_fd_get(fd));
  if (!h || h->kind != VFS_HANDLE_SOCKET) return -1;
  struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
  if (!addr || addrlen < sizeof(struct b1nix_sockaddr_in)) return -1;
  s->peer = *(const struct b1nix_sockaddr_in *)addr;
  s->connected = 1;
  if (s->type == B1NIX_SOCK_STREAM) {
    struct ipv4_addr dst_ip;
    dst_ip.bytes[0] = s->peer.sin_addr & 0xFF;
    dst_ip.bytes[1] = (s->peer.sin_addr >> 8) & 0xFF;
    dst_ip.bytes[2] = (s->peer.sin_addr >> 16) & 0xFF;
    dst_ip.bytes[3] = (s->peer.sin_addr >> 24) & 0xFF;
    s->tcp_conn = tcp_connect(dst_ip, (s->peer.sin_port << 8) | (s->peer.sin_port >> 8));
    if (!s->tcp_conn) return -1;
  }
  return 0;
}

isize vfs_socket_send(int fd, const void *buf, usize len, int flags) {
  struct vfs_handle *h = get_handle_by_idx(scheduler_fd_get(fd));
  if (!h || h->kind != VFS_HANDLE_SOCKET) return -1;
  return vfs_socket_send_h(h, buf, len, flags);
}

isize vfs_socket_recv(int fd, void *buf, usize len, int flags) {
  struct vfs_handle *h = get_handle_by_idx(scheduler_fd_get(fd));
  if (!h || h->kind != VFS_HANDLE_SOCKET) return -1;
  return vfs_socket_recv_h(h, buf, len, flags);
}

void vfs_socket_push_udp(u16 local_port, const void *data, usize len) {
  for (int i = 0; i < MAX_UDP_BINDINGS; i++) {
    if (udp_bindings[i].used && udp_bindings[i].port == local_port) {
      int h_idx = udp_bindings[i].h_idx;
      struct vfs_handle *h = get_handle_by_idx(h_idx);
      if (!h || !h->used || h->kind != VFS_HANDLE_SOCKET) {
        udp_bindings[i].used = 0;
        continue;
      }
      struct vfs_socket_state *s = (struct vfs_socket_state *)h->private_data;
      usize copy = (len > sizeof(s->recv_buf)) ? sizeof(s->recv_buf) : len;
      memcpy(s->recv_buf, data, copy);
      s->recv_len = copy;
      return;
    }
  }
}
