#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <stdlib.h>
#include <string.h>

#define UNIX_RB_SIZE 4096

struct unix_socket_data {
  struct vfs_socket_state *socket;
  struct unix_socket_data *peer;
  struct unix_socket_data *backlog[16];
  int backlog_count;
  int backlog_max;
  
  char *rb_buffer;
  usize rb_head;
  usize rb_tail;
  usize rb_count;
  
  volatile int lock;
};

static void unix_lock(struct unix_socket_data *u) {
  while (__atomic_test_and_set(&u->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
}

static void unix_unlock(struct unix_socket_data *u) {
  __atomic_clear(&u->lock, __ATOMIC_RELEASE);
}

int unix_init_state(struct vfs_socket_state *s) {
  struct unix_socket_data *u = kzalloc(sizeof(struct unix_socket_data));
  if (!u) return -ENOMEM;
  u->socket = s;
  u->rb_buffer = kmalloc(UNIX_RB_SIZE);
  if (!u->rb_buffer) { kfree(u); return -ENOMEM; }
  s->unix_data = u;
  return 0;
}

void unix_free_state(struct vfs_socket_state *s) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  if (!u) return;
  if (u->rb_buffer) kfree(u->rb_buffer);
  kfree(u);
  s->unix_data = 0;
}

int unix_bind(struct vfs_socket_state *s, const struct b1nix_sockaddr_un *addr) {
  if (s->bound) return -EINVAL;
  
  /* Create VFS node */
  struct vfs_node *node = vfs_add_node(addr->sun_path, VFS_SOCKET, s, 0, 0);
  if (IS_ERR(node)) return (int)PTR_ERR(node);
  
  s->local.un = *addr;
  s->bound = 1;
  return 0;
}

int unix_listen(struct vfs_socket_state *s, int backlog) {
  if (!s->bound || s->domain != B1NIX_AF_UNIX) return -EINVAL;
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  u->backlog_max = (backlog > 16) ? 16 : backlog;
  if (u->backlog_max <= 0) u->backlog_max = 1;
  s->listening = 1;
  return 0;
}

int unix_connect(struct vfs_socket_state *s, const struct b1nix_sockaddr_un *addr) {
  struct vfs_node *peer_node = vfs_find_node(addr->sun_path);
  if (IS_ERR(peer_node)) return -ECONNREFUSED;
  if (peer_node->inode->type != VFS_SOCKET) { vfs_node_put(peer_node); return -ENOTSOCK; }
  
  struct vfs_socket_state *peer_s = (struct vfs_socket_state *)peer_node->inode->data;
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  struct unix_socket_data *peer_u = (struct unix_socket_data *)peer_s->unix_data;
  
  if (s->type == B1NIX_SOCK_STREAM) {
    if (!peer_s->listening) { vfs_node_put(peer_node); return -ECONNREFUSED; }
    unix_lock(peer_u);
    if (peer_u->backlog_count >= peer_u->backlog_max) {
      unix_unlock(peer_u);
      vfs_node_put(peer_node);
      return -ECONNREFUSED;
    }
    peer_u->backlog[peer_u->backlog_count++] = u;
    unix_unlock(peer_u);
    
    /* Wake up peer for accept() */
    scheduler_wake_all(peer_s);
    scheduler_wake_all(vfs_poll_chan);
    
    /* Block until connected (simplified: just wait for peer to link us) */
    while (!s->connected) scheduler_yield();
  } else {
    /* DGRAM */
    u->peer = peer_u;
    s->connected = 1;
  }
  
  vfs_node_put(peer_node);
  return 0;
}

int unix_accept(struct vfs_socket_state *s, struct vfs_socket_state *new_s) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  struct unix_socket_data *new_u = (struct unix_socket_data *)new_s->unix_data;
  
  while (1) {
    unix_lock(u);
    if (u->backlog_count > 0) {
      struct unix_socket_data *client_u = u->backlog[0];
      for (int i = 0; i < u->backlog_count - 1; i++) u->backlog[i] = u->backlog[i+1];
      u->backlog_count--;
      unix_unlock(u);
      
      /* Link them */
      new_u->peer = client_u;
      client_u->peer = new_u;
      new_s->connected = 1;
      client_u->socket->connected = 1;
      
      return 0;
    }
    unix_unlock(u);
    scheduler_block_on(s);
  }
}

isize unix_send(struct vfs_socket_state *s, const void *buf, usize len) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  if (!s->connected || !u->peer) return -ENOTCONN;
  
  struct unix_socket_data *peer_u = u->peer;
  unix_lock(peer_u);
  
  usize free_space = UNIX_RB_SIZE - peer_u->rb_count;
  if (free_space == 0) {
    unix_unlock(peer_u);
    return -EAGAIN;
  }
  
  usize to_copy = (len < free_space) ? len : free_space;
  for (usize i = 0; i < to_copy; i++) {
    peer_u->rb_buffer[peer_u->rb_tail] = ((const char *)buf)[i];
    peer_u->rb_tail = (peer_u->rb_tail + 1) % UNIX_RB_SIZE;
  }
  peer_u->rb_count += to_copy;
  
  unix_unlock(peer_u);
  scheduler_wake_all(peer_u->socket);
  scheduler_wake_all(vfs_poll_chan);
  return (isize)to_copy;
}

isize unix_recv(struct vfs_socket_state *s, void *buf, usize len) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  
  while (1) {
    unix_lock(u);
    if (u->rb_count > 0) {
      usize to_copy = (len < u->rb_count) ? len : u->rb_count;
      for (usize i = 0; i < to_copy; i++) {
        ((char *)buf)[i] = u->rb_buffer[u->rb_head];
        u->rb_head = (u->rb_head + 1) % UNIX_RB_SIZE;
      }
      u->rb_count -= to_copy;
      unix_unlock(u);
      
      /* Wake up anyone waiting to send more data */
      scheduler_wake_all(u->socket);
      scheduler_wake_all(vfs_poll_chan);
      
      return (isize)to_copy;
    }
    unix_unlock(u);
    if (s->type == B1NIX_SOCK_STREAM && !s->connected) return 0;
    scheduler_block_on(s);
  }
}

int unix_poll(struct vfs_socket_state *s, struct b1nix_pollfd *pfd) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  pfd->revents = 0;
  if (u->rb_count > 0) pfd->revents |= B1NIX_POLLIN;
  if (s->type == B1NIX_SOCK_STREAM && !s->connected) pfd->revents |= B1NIX_POLLHUP;
  
  /* Check if peer has space for writing */
  if (s->connected && u->peer) {
    struct unix_socket_data *peer_u = u->peer;
    if (peer_u->rb_count < UNIX_RB_SIZE) pfd->revents |= B1NIX_POLLOUT;
  } else if (s->type == B1NIX_SOCK_DGRAM) {
      pfd->revents |= B1NIX_POLLOUT;
  }
  
  if (s->listening && u->backlog_count > 0) pfd->revents |= B1NIX_POLLIN;
  
  return 0;
}
