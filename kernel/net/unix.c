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
  /* Lifetime: a unix_socket_data outlives its own socket as long as a PEER (or
   * a listener backlog slot) still points at it. Each such pointer is a counted
   * reference; the struct is freed when the count hits 0. This is what makes
   * the connected-peer dereference in unix_send/unix_recv safe across a close
   * on the other end (the old code freed it under the live peer → UAF). */
  int refcount;
};

static void unix_lock(struct unix_socket_data *u) {
  while (__atomic_test_and_set(&u->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
}

static void unix_unlock(struct unix_socket_data *u) {
  __atomic_clear(&u->lock, __ATOMIC_RELEASE);
}

static void unix_data_get(struct unix_socket_data *u) {
  if (u) __atomic_add_fetch(&u->refcount, 1, __ATOMIC_RELAXED);
}

static void unix_data_put(struct unix_socket_data *u) {
  if (!u) return;
  if (__atomic_sub_fetch(&u->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
    if (u->rb_buffer) kfree(u->rb_buffer);
    kfree(u);
  }
}

int unix_init_state(struct vfs_socket_state *s) {
  struct unix_socket_data *u = kzalloc(sizeof(struct unix_socket_data));
  if (!u) return -ENOMEM;
  u->socket = s;
  u->refcount = 1; /* the owning socket's reference */
  u->rb_buffer = kmalloc(UNIX_RB_SIZE);
  if (!u->rb_buffer) { kfree(u); return -ENOMEM; }
  s->unix_data = u;
  return 0;
}

void unix_free_state(struct vfs_socket_state *s) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  if (!u) return;
  s->unix_data = 0;

  /* Detach from a connected peer: clear OUR forward pointer and the peer's
   * back pointer, mark the peer disconnected, and wake it so its blocked
   * recv/send/poll observes the hangup. Each direction was a counted ref. */
  unix_lock(u);
  struct unix_socket_data *peer = u->peer;
  u->peer = 0;
  /* Snapshot still-pending backlog connectors (never accepted) to drop. */
  struct unix_socket_data *pending[16];
  int npending = 0;
  for (int i = 0; i < u->backlog_count && npending < 16; i++)
    pending[npending++] = u->backlog[i];
  u->backlog_count = 0;
  unix_unlock(u);

  if (peer) {
    unix_lock(peer);
    int linked = (peer->peer == u);
    if (linked) {
      peer->peer = 0;
      if (peer->socket) peer->socket->connected = 0;
    }
    unix_unlock(peer);
    if (linked) {
      if (peer->socket) scheduler_wake_all(peer->socket);
      scheduler_wake_all(vfs_poll_chan);
      unix_data_put(u);    /* drop the peer's reference on us */
    }
    unix_data_put(peer);   /* drop our reference on the peer */
  }

  for (int i = 0; i < npending; i++)
    unix_data_put(pending[i]); /* backlog slot references */

  unix_data_put(u);        /* drop the owning socket's reference */
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
    unix_data_get(u); /* the backlog slot holds a reference on us */
    peer_u->backlog[peer_u->backlog_count++] = u;
    unix_unlock(peer_u);

    /* Wake up peer for accept() */
    scheduler_wake_all(peer_s);
    scheduler_wake_all(vfs_poll_chan);

    /* Block until connected (simplified: just wait for peer to link us) */
    while (!s->connected) {
      if (scheduler_signal_pending()) {
        /* Splice ourselves out of the peer's backlog so a later accept does
         * not hand out a connector that has bailed (stale/UAF entry). */
        unix_lock(peer_u);
        for (int i = 0; i < peer_u->backlog_count; i++) {
          if (peer_u->backlog[i] == u) {
            for (int j = i; j < peer_u->backlog_count - 1; j++)
              peer_u->backlog[j] = peer_u->backlog[j + 1];
            peer_u->backlog_count--;
            unix_data_put(u); /* drop the backlog slot's reference */
            break;
          }
        }
        int still_connected = s->connected;
        unix_unlock(peer_u);
        vfs_node_put(peer_node);
        /* If accept linked us between the check and the splice, honor it. */
        return still_connected ? 0 : -ERESTARTSYS;
      }
      scheduler_yield();
    }
  } else {
    /* DGRAM */
    unix_data_get(peer_u); /* u->peer holds a reference on the peer */
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

      /* Link them. The backlog slot's reference on client_u transfers to
       * new_u->peer (no extra get); client_u->peer takes a fresh reference on
       * new_u. */
      new_u->peer = client_u;
      unix_data_get(new_u);
      client_u->peer = new_u;
      new_s->connected = 1;
      if (client_u->socket)
        client_u->socket->connected = 1;

      return 0;
    }
    unix_unlock(u);
    if (scheduler_signal_pending()) return -ERESTARTSYS;
    scheduler_block_on(s);
  }
}

isize unix_send(struct vfs_socket_state *s, const void *buf, usize len) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;

  /* Pin the peer for the duration of the write. Reading u->peer and taking the
   * reference under u's lock is what makes this safe: the peer closing on
   * another CPU clears u->peer and drops the link reference in unix_free_state,
   * but our own reference keeps the peer's data (and its ring buffer) alive
   * until we are done — without this the old code dereferenced freed memory. */
  unix_lock(u);
  struct unix_socket_data *peer_u = u->peer;
  if (!s->connected || !peer_u) {
    unix_unlock(u);
    return -ENOTCONN;
  }
  unix_data_get(peer_u);
  unix_unlock(u);

  unix_lock(peer_u);
  usize free_space = UNIX_RB_SIZE - peer_u->rb_count;
  if (free_space == 0) {
    unix_unlock(peer_u);
    unix_data_put(peer_u);
    return -EAGAIN;
  }

  usize to_copy = (len < free_space) ? len : free_space;
  for (usize i = 0; i < to_copy; i++) {
    peer_u->rb_buffer[peer_u->rb_tail] = ((const char *)buf)[i];
    peer_u->rb_tail = (peer_u->rb_tail + 1) % UNIX_RB_SIZE;
  }
  peer_u->rb_count += to_copy;

  struct vfs_socket_state *peer_sock = peer_u->socket;
  unix_unlock(peer_u);
  if (peer_sock) scheduler_wake_all(peer_sock);
  scheduler_wake_all(vfs_poll_chan);
  unix_data_put(peer_u);
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
    if (scheduler_signal_pending()) return -ERESTARTSYS;
    scheduler_block_on(s);
  }
}

int unix_poll(struct vfs_socket_state *s, struct b1nix_pollfd *pfd) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  pfd->revents = 0;
  if (u->rb_count > 0) pfd->revents |= B1NIX_POLLIN;
  if (s->type == B1NIX_SOCK_STREAM && !s->connected) pfd->revents |= B1NIX_POLLHUP;
  
  /* Check if peer has space for writing. Pin the peer while we read it so a
   * concurrent close cannot free it under us. */
  if (s->connected) {
    unix_lock(u);
    struct unix_socket_data *peer_u = u->peer;
    if (peer_u) unix_data_get(peer_u);
    unix_unlock(u);
    if (peer_u) {
      if (peer_u->rb_count < UNIX_RB_SIZE) pfd->revents |= B1NIX_POLLOUT;
      unix_data_put(peer_u);
    }
  } else if (s->type == B1NIX_SOCK_DGRAM) {
      pfd->revents |= B1NIX_POLLOUT;
  }
  
  if (s->listening && u->backlog_count > 0) pfd->revents |= B1NIX_POLLIN;
  
  return 0;
}
