#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/serial.h>
#include <b1nix/syscall.h>
#include <stdlib.h>
#include <string.h>

#define UNIX_RB_SIZE 4096
#define UNIX_CONTROL_SLOTS 16
#define UNIX_MSG_SLOTS 32

struct unix_control {
  int used;
  u64 seq;
  struct vfs_handle *handles[VFS_SCM_MAX_FDS];
  usize nhandles;
  struct b1nix_ucred cred;
  int has_cred;
};

struct unix_socket_data {
  struct vfs_socket_state *socket;
  /* Credentials of the task that created this socket, captured once at
   * creation. SO_PEERCRED reports the PEER's copy — an authentic identity the
   * peer cannot forge, unlike anything it sends in the data stream. A crash
   * handler uses it to decide whether the process on the other end of its
   * socket is one it is responsible for. */
  struct b1nix_ucred owner;
  struct unix_socket_data *peer;
  /* This socket has been connected at least once. It distinguishes "the peer
   * hung up" (POLLHUP) from "never connected", which a listening socket is
   * permanently in — reporting HUP on a listener tells an event loop its
   * listening socket died. */
  int had_peer;
  struct unix_socket_data *backlog[16];
  int backlog_count;
  int backlog_max;
  /* Set while this socket sits, not-yet-accepted, in a listener's backlog[]
   * (unix_connect). Lets unix_free_state splice a closing connector out of the
   * listener's backlog instead of leaving a dangling entry that a later
   * accept() hands out (UAF the moment anyone touches it, e.g. unix_lock). */
  struct unix_socket_data *pending_listener;
  
  char *rb_buffer;
  usize rb_head;
  usize rb_tail;
  usize rb_count;
  u64 read_seq;
  u64 write_seq;
  struct unix_control control[UNIX_CONTROL_SLOTS];

  /* SOCK_SEQPACKET message boundaries: one length per datagram sitting in the
   * ring buffer, in arrival order. A stream socket leaves this empty and reads
   * the ring as an undivided byte stream. */
  u32 msg_len[UNIX_MSG_SLOTS];
  u8 msg_head, msg_tail, msg_count;

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
    for (int i = 0; i < UNIX_CONTROL_SLOTS; i++)
      for (usize j = 0; j < u->control[i].nhandles; j++)
        if (u->control[i].handles[j])
          vfs_handle_release(u->control[i].handles[j]);
    if (u->rb_buffer) kfree(u->rb_buffer);
    kfree(u);
  }
}

int unix_init_state(struct vfs_socket_state *s) {
  struct unix_socket_data *u = kzalloc(sizeof(struct unix_socket_data));
  if (!u) return -ENOMEM;
  u->socket = s;
  u->refcount = 1; /* the owning socket's reference */
  u->owner.pid = (int)scheduler_get_pid();
  {
    const struct cred *c = scheduler_get_current_cred();
    u->owner.uid = c ? c->uid : 0;
    u->owner.gid = c ? c->gid : 0;
  }
  u->rb_buffer = kmalloc(UNIX_RB_SIZE);
  if (!u->rb_buffer) { kfree(u); return -ENOMEM; }
  s->unix_data = u;
  return 0;
}

/* socketpair(): cross-connect two freshly-created AF_UNIX sockets without going
 * through bind/listen/connect/accept (no filesystem name). Each side becomes
 * the other's connected peer, mirroring what unix_accept establishes. Each
 * direction is a counted reference, so a close on one end runs through
 * unix_free_state exactly as a path-connected pair would (waking the survivor,
 * releasing any in-flight SCM_RIGHTS handles). Both states must already be
 * AF_UNIX with unix_init_state run. */
void unix_link_pair(struct vfs_socket_state *a, struct vfs_socket_state *b) {
  struct unix_socket_data *ua = (struct unix_socket_data *)a->unix_data;
  struct unix_socket_data *ub = (struct unix_socket_data *)b->unix_data;
  ua->peer = ub;
  unix_data_get(ub); /* a->peer holds a reference on b */
  ub->peer = ua;
  unix_data_get(ua); /* b->peer holds a reference on a */
  a->connected = 1;
  b->connected = 1;
  ua->had_peer = 1;
  ub->had_peer = 1;
}

void unix_free_state(struct vfs_socket_state *s) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  if (!u) return;
  s->unix_data = 0;

  /* A bound socket left its vfs node's inode->data pointing at this state
   * (unix_bind). The file itself persists after close (Linux semantics), but
   * the pointer must be severed NOW: a later connect() dereferences
   * inode->data, and after the kfree(s) below that's a UAF → GP fault (seen
   * as clients connecting to a crashed displayd's stale socket). Cleared
   * only if it still points at us — a new socket may have rebound the path. */
  if (s->bound && s->local.un.sun_path[0]) {
    struct vfs_node *bn = vfs_find_node(s->local.un.sun_path);
    if (!IS_ERR(bn)) {
      if (bn->inode && bn->inode->type == VFS_SOCKET &&
          bn->inode->data == (void *)s)
        bn->inode->data = 0;
      vfs_node_put(bn);
    }
  }

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
      /* Also wake anyone blocked in unix_send_control waiting for space in OUR
       * ring buffer (they park on our own socket, the buffer-owner) so they see
       * the hangup and return ENOTCONN instead of sleeping forever. */
      if (u->socket) scheduler_wake_all(u->socket);
      scheduler_wake_all(vfs_poll_chan);
      unix_data_put(u);    /* drop the peer's reference on us */
    }
    unix_data_put(peer);   /* drop our reference on the peer */
  }

  for (int i = 0; i < npending; i++)
    unix_data_put(pending[i]); /* backlog slot references */

  /* If we are ourselves a still-pending connector (unix_connect enqueued us
   * into a listener's backlog and nobody has accept()ed us yet), splice
   * ourselves out of that backlog. Without this, closing a connecting socket
   * before accept() leaves a dangling pointer in the listener's backlog[]
   * that a later accept() hands out — accept() then writes through it and
   * any subsequent unix_lock() on it faults on freed memory. */
  struct unix_socket_data *listener = u->pending_listener;
  u->pending_listener = 0;
  if (listener) {
    unix_lock(listener);
    int spliced = 0;
    for (int i = 0; i < listener->backlog_count; i++) {
      if (listener->backlog[i] == u) {
        for (int j = i; j < listener->backlog_count - 1; j++)
          listener->backlog[j] = listener->backlog[j + 1];
        listener->backlog_count--;
        spliced = 1;
        break;
      }
    }
    unix_unlock(listener);
    if (spliced) unix_data_put(u); /* drop the backlog slot's reference on us */
    unix_data_put(listener);       /* drop the pending_listener backref */
  }

  unix_data_put(u);        /* drop the owning socket's reference */
}

/* SO_PEERCRED: the identity of the process that created the socket at the other
 * end of this connection. Only a connected socket has one. */
/* Bytes a reader could take right now — what FIONREAD reports. A SEQPACKET
 * socket answers with its next message's length, since that is all one read
 * can return. */
usize unix_bytes_available(struct vfs_socket_state *s) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  if (!u)
    return 0;
  unix_lock(u);
  usize n = u->rb_count;
  if (s->type == B1NIX_SOCK_SEQPACKET && u->msg_count)
    n = u->msg_len[u->msg_head];
  unix_unlock(u);
  return n;
}

int unix_peer_cred(struct vfs_socket_state *s, struct b1nix_ucred *out) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  if (!u || !out)
    return -EINVAL;
  unix_lock(u);
  struct unix_socket_data *peer = u->peer;
  if (!peer) {
    unix_unlock(u);
    return -ENOTCONN;
  }
  *out = peer->owner;
  unix_unlock(u);
  return 0;
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

int unix_connect(struct vfs_socket_state *s, const struct b1nix_sockaddr_un *addr,
                 int nonblock) {
  /* /dev/log: the kernel is the syslog sink (no userspace syslogd). musl's
   * syslog() connect()s a SOCK_DGRAM here; accept it and forward later sends to
   * the serial console instead of requiring a bound peer socket. */
  if (addr->sun_path[0] &&
      strcmp(addr->sun_path, "/dev/log") == 0) {
    s->syslog_sink = 1;
    s->connected = 1;
    return 0;
  }
  struct vfs_node *peer_node = vfs_find_node(addr->sun_path);
  if (IS_ERR(peer_node)) return -ECONNREFUSED;
  if (peer_node->inode->type != VFS_SOCKET) { vfs_node_put(peer_node); return -ENOTSOCK; }
  
  struct vfs_socket_state *peer_s = (struct vfs_socket_state *)peer_node->inode->data;
  /* The socket file outlives its socket: after the owner closes (or crashes),
   * teardown clears inode->data. Linux semantics: ECONNREFUSED, not a deref. */
  if (!peer_s) { vfs_node_put(peer_node); return -ECONNREFUSED; }
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  struct unix_socket_data *peer_u = (struct unix_socket_data *)peer_s->unix_data;
  if (!peer_u) { vfs_node_put(peer_node); return -ECONNREFUSED; }

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
    unix_data_get(peer_u); /* pending_listener back-reference: lets a close()
                             * while still pending splice us out instead of
                             * leaving a dangling backlog entry */
    u->pending_listener = peer_u;
    unix_unlock(peer_u);

    /* Wake up peer for accept() */
    scheduler_wake_all(peer_s);
    scheduler_wake_all(vfs_poll_chan);

    /* O_NONBLOCK: the connection is queued in the listener's backlog, and
     * POSIX says report EINPROGRESS now rather than waiting for accept().
     * Blocking here regardless is how a non-blocking connect() to a listener
     * that had not accepted yet wedged the caller for good. */
    if (nonblock && !s->connected) {
      vfs_node_put(peer_node);
      return -EINPROGRESS;
    }

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
            u->pending_listener = 0;
            unix_data_put(peer_u); /* drop the pending_listener backref (self-spliced) */
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
    u->had_peer = 1;
  }

  vfs_node_put(peer_node);
  return 0;
}

int unix_accept(struct vfs_socket_state *s, struct vfs_socket_state *new_s,
                int nonblock) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  struct unix_socket_data *new_u = (struct unix_socket_data *)new_s->unix_data;

  while (1) {
    unix_lock(u);
    if (u->backlog_count == 0 && nonblock) {
      /* O_NONBLOCK listener with an empty backlog: report EAGAIN instead of
       * blocking. displayd's pre-poll accept drain froze the compositor for
       * seconds at a time without this. */
      unix_unlock(u);
      return -EAGAIN;
    }
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
      new_u->had_peer = 1;
      client_u->had_peer = 1;
      if (client_u->socket)
        client_u->socket->connected = 1;

      /* Accepted: drop the pending_listener backref (client_u is no longer
       * sitting in our backlog, so a close() racing in now takes the normal
       * peer-detach path in unix_free_state instead of the backlog-splice
       * one). */
      client_u->pending_listener = 0;
      unix_data_put(u);

      return 0;
    }
    unix_unlock(u);
    /* SMP-safe wait: publish BLOCKED before re-testing the backlog so a
     * connector's wake (scheduler_wake_all(listener) in unix_connect) racing
     * between the check above and the block cannot be lost. */
    scheduler_wait_prepare(s);
    unix_lock(u);
    int have_conn = (u->backlog_count > 0);
    unix_unlock(u);
    if (have_conn) {
      scheduler_wait_cancel();
      continue;
    }
    if (scheduler_signal_pending()) {
      scheduler_wait_cancel();
      return -ERESTARTSYS;
    }
    scheduler_wait_commit();
  }
}

isize unix_send_control(struct vfs_socket_state *s, const void *buf, usize len,
                        struct vfs_handle **handles, usize nhandles,
                        const struct b1nix_ucred *cred, int nonblock) {
  /* /dev/log syslog sink: forward the datagram to the serial console prefixed
   * with "/dev/log: " (matches the M54-LOG smoke expectation). No peer/ring. */
  if (s->syslog_sink) {
    char line[512];
    usize n = (buf && len < sizeof(line) - 1) ? len : (buf ? sizeof(line) - 1 : 0);
    if (n) {
      if (syscall_copyin(line, buf, n) < 0)
        return -EFAULT;
    }
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      n--;
    line[n] = '\0';
    serial_write("/dev/log: ");
    serial_write(line);
    serial_write("\n");
    return (isize)len;
  }
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  if ((nhandles || cred) && len == 0)
    return -EINVAL;

retry:
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
    /* Buffer full. POSIX: a blocking socket waits for the reader to drain;
     * only O_NONBLOCK returns EAGAIN. Block on the peer (buffer-owner) socket:
     * unix_recv_control wakes peer_u->socket after draining, and unix_free_state
     * wakes it on hangup. Publish BLOCKED before the final full-recheck so a
     * drain racing in on another CPU can't be lost. */
    struct vfs_socket_state *psock = peer_u->socket;
    unix_unlock(peer_u);
    if (nonblock) {
      unix_data_put(peer_u);
      return -EAGAIN;
    }
    scheduler_wait_prepare(psock);
    unix_lock(peer_u);
    int still_full = (peer_u->rb_count >= UNIX_RB_SIZE);
    unix_unlock(peer_u);
    unix_data_put(peer_u);
    if (!s->connected) {            /* peer hung up → retry reports ENOTCONN */
      scheduler_wait_cancel();
      goto retry;
    }
    if (!still_full) {              /* space freed between check and prepare */
      scheduler_wait_cancel();
      goto retry;
    }
    if (scheduler_signal_pending()) {
      scheduler_wait_cancel();
      return -ERESTARTSYS;
    }
    scheduler_wait_commit();        /* block until drained / hangup */
    goto retry;
  }

  /* SO_PASSCRED on the receiving end: attach this sender's credentials even
   * though the sender did not ask to send any. */
  struct b1nix_ucred auto_cred;
  if (!cred && peer_u->socket && peer_u->socket->so_passcred) {
    auto_cred.pid = (int)scheduler_get_pid();
    const struct cred *c = scheduler_get_current_cred();
    auto_cred.uid = c ? c->uid : 0;
    auto_cred.gid = c ? c->gid : 0;
    cred = &auto_cred;
  }

  int control_slot = -1;
  if (nhandles || cred) {
    for (int i = 0; i < UNIX_CONTROL_SLOTS; i++)
      if (!peer_u->control[i].used) {
        control_slot = i;
        break;
      }
    if (control_slot < 0) {
      unix_unlock(peer_u);
      unix_data_put(peer_u);
      return -ENOBUFS;
    }
  }

  int seqpacket = (s->type == B1NIX_SOCK_SEQPACKET);
  if (seqpacket) {
    /* A SOCK_SEQPACKET write is all-or-nothing: a message that cannot fit
     * whole is either too large ever (EMSGSIZE) or has to wait for the reader
     * to drain, exactly as it does on Linux. */
    if (len > UNIX_RB_SIZE) {
      unix_unlock(peer_u);
      unix_data_put(peer_u);
      return -EMSGSIZE;
    }
    if (len > free_space || peer_u->msg_count >= UNIX_MSG_SLOTS) {
      struct vfs_socket_state *psock = peer_u->socket;
      unix_unlock(peer_u);
      if (nonblock) {
        unix_data_put(peer_u);
        return -EAGAIN;
      }
      scheduler_wait_prepare(psock);
      unix_data_put(peer_u);
      if (scheduler_signal_pending()) {
        scheduler_wait_cancel();
        return -ERESTARTSYS;
      }
      scheduler_wait_commit();
      goto retry;
    }
  }

  usize to_copy = (len < free_space) ? len : free_space;
  if (control_slot >= 0) {
    struct unix_control *ctl = &peer_u->control[control_slot];
    memset(ctl, 0, sizeof(*ctl));
    ctl->used = 1;
    ctl->seq = peer_u->write_seq;
    ctl->nhandles = nhandles;
    for (usize i = 0; i < nhandles; i++)
      ctl->handles[i] = handles[i];
    if (cred) {
      ctl->cred = *cred;
      ctl->has_cred = 1;
    }
  }
  for (usize i = 0; i < to_copy; i++) {
    peer_u->rb_buffer[peer_u->rb_tail] = ((const char *)buf)[i];
    peer_u->rb_tail = (peer_u->rb_tail + 1) % UNIX_RB_SIZE;
  }
  peer_u->rb_count += to_copy;
  peer_u->write_seq += to_copy;
  if (seqpacket) {
    peer_u->msg_len[peer_u->msg_tail] = (u32)to_copy;
    peer_u->msg_tail = (u8)((peer_u->msg_tail + 1) % UNIX_MSG_SLOTS);
    peer_u->msg_count++;
  }

  struct vfs_socket_state *peer_sock = peer_u->socket;
  unix_unlock(peer_u);
  if (peer_sock) scheduler_wake_all(peer_sock);
  scheduler_wake_all(vfs_poll_chan);
  unix_data_put(peer_u);
  return (isize)to_copy;
}

isize unix_send(struct vfs_socket_state *s, const void *buf, usize len,
                int nonblock) {
  return unix_send_control(s, buf, len, 0, 0, 0, nonblock);
}

isize unix_recv_control(struct vfs_socket_state *s, void *buf, usize len,
                        int flags, struct vfs_handle **handles,
                        usize *nhandles, struct b1nix_ucred *cred,
                        int *has_cred) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  if (nhandles)
    *nhandles = 0;
  if (has_cred)
    *has_cred = 0;
  
  while (1) {
    unix_lock(u);
    if (u->rb_count > 0) {
      int seqpacket = (s->type == B1NIX_SOCK_SEQPACKET);
      usize avail = u->rb_count;
      if (seqpacket) {
        /* Exactly one message is visible to this call, and anything the caller
         * has no room for is discarded with it (POSIX MSG_TRUNC semantics). */
        avail = u->msg_count ? u->msg_len[u->msg_head] : u->rb_count;
      }
      usize to_copy = (len < avail) ? len : avail;
      struct unix_control *ctl = 0;
      for (int i = 0; i < UNIX_CONTROL_SLOTS; i++) {
        struct unix_control *candidate = &u->control[i];
        if (!candidate->used)
          continue;
        if (candidate->seq > u->read_seq &&
            candidate->seq < u->read_seq + to_copy)
          to_copy = (usize)(candidate->seq - u->read_seq);
        if (candidate->seq == u->read_seq)
          ctl = candidate;
      }
      for (usize i = 0; i < to_copy; i++) {
        usize idx = (u->rb_head + i) % UNIX_RB_SIZE;
        ((char *)buf)[i] = u->rb_buffer[idx];
      }
      usize consume = to_copy;
      if (seqpacket && !(flags & B1NIX_MSG_PEEK))
        consume = avail; /* the rest of the message is dropped, not re-read */
      if (!(flags & B1NIX_MSG_PEEK))
        u->rb_head = (u->rb_head + consume) % UNIX_RB_SIZE;
      if (!(flags & B1NIX_MSG_PEEK)) {
        u->rb_count -= consume;
        u->read_seq += consume;
        if (seqpacket && u->msg_count) {
          u->msg_head = (u8)((u->msg_head + 1) % UNIX_MSG_SLOTS);
          u->msg_count--;
        }
        if (ctl) {
          if (handles && nhandles) {
            for (usize i = 0; i < ctl->nhandles; i++) {
              handles[i] = ctl->handles[i];
              ctl->handles[i] = 0;
            }
            *nhandles = ctl->nhandles;
          } else {
            for (usize i = 0; i < ctl->nhandles; i++)
              if (ctl->handles[i])
                vfs_handle_release(ctl->handles[i]);
          }
          if (ctl->has_cred && cred && has_cred) {
            *cred = ctl->cred;
            *has_cred = 1;
          }
          memset(ctl, 0, sizeof(*ctl));
        }
      }
      unix_unlock(u);
      
      /* Wake up anyone waiting to send more data */
      scheduler_wake_all(u->socket);
      scheduler_wake_all(vfs_poll_chan);
      
      return (isize)to_copy;
    }
    unix_unlock(u);
    if ((s->type == B1NIX_SOCK_STREAM || s->type == B1NIX_SOCK_SEQPACKET) &&
        !s->connected)
      return 0;
    if (flags & B1NIX_MSG_DONTWAIT) return -EAGAIN;
    /* SMP-safe wait: publish BLOCKED on our socket, then re-test under the lock
     * so a sender's wake (scheduler_wake_all(u->socket) after a write/close)
     * racing between the check above and the block cannot be lost. */
    scheduler_wait_prepare(s);
    unix_lock(u);
    int have_data = (u->rb_count > 0);
    int disconnected =
        ((s->type == B1NIX_SOCK_STREAM || s->type == B1NIX_SOCK_SEQPACKET) &&
         !s->connected);
    unix_unlock(u);
    if (have_data || disconnected) {
      scheduler_wait_cancel();
      continue;
    }
    if (scheduler_signal_pending()) {
      scheduler_wait_cancel();
      return -ERESTARTSYS;
    }
    scheduler_wait_commit();
  }
}

isize unix_recv(struct vfs_socket_state *s, void *buf, usize len) {
  return unix_recv_control(s, buf, len, 0, 0, 0, 0, 0);
}

int unix_poll(struct vfs_socket_state *s, struct b1nix_pollfd *pfd) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  pfd->revents = 0;
  if (u->rb_count > 0) pfd->revents |= B1NIX_POLLIN;
  /* POLLHUP means a peer that WAS there is gone. A listening socket has no
   * peer by definition and a fresh socket has not had one yet; reporting HUP
   * for either tells an event loop that its socket died, which is how sway's
   * IPC listener ended up unusable — every dispatch saw HANGUP on the one fd
   * it was waiting to accept connections on. */
  if (s->type == B1NIX_SOCK_STREAM && !s->listening && !s->connected &&
      u->had_peer)
    pfd->revents |= B1NIX_POLLHUP;
  
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
