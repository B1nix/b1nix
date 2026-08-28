#include <b1nix/vfs.h>
#include <b1nix/bootinfo.h>
#include <b1nix/ktime.h>
#include <b1nix/rtc.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/serial.h>
#include <b1nix/syscall.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <stdlib.h>
#include <string.h>

/* Linux gives an AF_UNIX socket about 200 KiB of buffer (net.core.wmem_default,
 * 212992). Four kilobytes here meant chromium's sandbox IPC — SOCK_SEQPACKET,
 * whose messages must fit whole — got EMSGSIZE on anything larger, and its Mojo
 * channels lived in permanent partial-write and EAGAIN cycles.
 *
 * The buffer is per socket and kmalloc'd, so the right size depends on how much
 * memory the machine has: a desktop session holds hundreds of these at once,
 * and 208 KiB apiece is a different proposition on a 512 MiB self-host guest
 * than on a 4 GiB one. It is therefore derived from RAM between a floor of the
 * old 64 KiB and Linux's own figure, and `b1nix.unix-buf-kb=N` overrides it. */
#define UNIX_RB_MIN (64 * 1024)
#define UNIX_RB_MAX 212992
static usize unix_rb_size_v;

static usize unix_rb_size(void) {
  if (unix_rb_size_v)
    return unix_rb_size_v;
  char buf[24];
  usize want = 0;
  if (bootinfo_get_kv("b1nix.unix-buf-kb", buf, sizeof(buf)) && buf[0] >= '0' &&
      buf[0] <= '9') {
    usize kb = 0;
    for (const char *p = buf; *p >= '0' && *p <= '9'; p++)
      kb = kb * 10u + (usize)(*p - '0');
    want = kb * 1024u;
  } else {
    /* One eight-thousandth of RAM: the floor up to 512 MiB, Linux's figure
     * from 1.7 GiB up. */
    want = (usize)(pmm_total_usable_memory() / 8192ULL);
  }
  if (want < UNIX_RB_MIN)
    want = UNIX_RB_MIN;
  if (want > UNIX_RB_MAX)
    want = UNIX_RB_MAX;
  unix_rb_size_v = want;
  return unix_rb_size_v;
}
/* In-flight messages carrying descriptors. A slot frees only once the reader
 * reaches it, and chromium sends them in bursts, so sixteen ran out and the
 * sender saw ENOBUFS — an error Linux never returns here, and one that Mojo
 * treats as a dead channel rather than a retry. */
/* Ancillary-data slots per socket. One is taken by every message that carries
 * descriptors or credentials — and with SO_PASSCRED set, which is what a bus
 * daemon does to every connection, that is EVERY message. Thirty-two of them
 * is a handful of round trips on a busy connection. */
#define UNIX_CONTROL_SLOTS 256
#define UNIX_MSG_SLOTS 64
/* Accept-queue depth. Linux's somaxconn is 4096, but that queue is allocated;
 * ours is an array inside every AF_UNIX socket and is also snapshotted onto the
 * stack when a listener closes, so the depth is bounded by what both can carry
 * cheaply — 64 pointers is 512 bytes in either place. */
#define UNIX_BACKLOG_MAX 64

/* Wall-clock microseconds, the unit SCM_TIMESTAMP's struct timeval carries. */
static u64 unix_wallclock_usec(void) { return rtc_now_unix_nanos() / 1000ull; }

struct unix_control {
  int used;
  u64 seq;
  /* Send order, so two control blocks that start at the same byte offset are
   * still handed over in the order they were sent. Only a zero-length message
   * can share an offset with the message after it. */
  u64 order;
  struct vfs_handle *handles[VFS_SCM_MAX_FDS];
  usize nhandles;
  struct b1nix_ucred cred;
  int has_cred;
  /* The credentials were attached by the kernel rather than named by the
   * sender. Those are reported only to a receiver that set SO_PASSCRED —
   * handing an unasked-for SCM_CREDENTIALS to a caller with a small control
   * buffer truncates its ancillary data instead. A sender that put the
   * credentials there itself is answering a question that was asked. */
  int cred_implicit;
  /* Wall-clock microseconds at which this message was placed in the receive
   * buffer, for SO_TIMESTAMP. Zero when the receiver had not asked. */
  u64 stamp_usec;
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
  /* The peer's credentials as they were when the connection was made.
   *
   * SO_PEERCRED answers "who is on the other end", and the answer must survive
   * the other end closing — Linux snapshots it at connect time for exactly that
   * reason. Reading the live peer instead returned ENOTCONN the moment a client
   * that had already been served exited, which a server asking who it just
   * served hits routinely. */
  struct b1nix_ucred peer_cred;
  int has_peer_cred;
  /* This socket has been connected at least once. It distinguishes "the peer
   * hung up" (POLLHUP) from "never connected", which a listening socket is
   * permanently in — reporting HUP on a listener tells an event loop its
   * listening socket died. */
  int had_peer;
  /* Accept queue. Linux's somaxconn is 4096 and a listener's queue is
   * allocated from it; this array is inline in every AF_UNIX socket, so the
   * depth is what a desktop session's busiest listener needs rather than that.
   * At sixteen, a compositor being connected to by a burst of clients refused
   * connections a Linux kernel would have queued. */
  struct unix_socket_data *backlog[UNIX_BACKLOG_MAX];
  int backlog_count;
  int backlog_max;
  /* Set while this socket sits, not-yet-accepted, in a listener's backlog[]
   * (unix_connect). Lets unix_free_state splice a closing connector out of the
   * listener's backlog instead of leaving a dangling entry that a later
   * accept() hands out (UAF the moment anyone touches it, e.g. unix_lock). */
  struct unix_socket_data *pending_listener;
  
  char *rb_buffer;
  usize rb_size; /* bytes in rb_buffer — unix_rb_size() at creation time */
  usize rb_head;
  usize rb_tail;
  usize rb_count;
  u64 read_seq;
  u64 write_seq;
  struct unix_control control[UNIX_CONTROL_SLOTS];
  /* Arrival stamp of the message the last recv handed over, for the
   * SCM_TIMESTAMP the syscall layer attaches. */
  u64 last_stamp_usec;

  /* SOCK_SEQPACKET message boundaries: one length per datagram sitting in the
   * ring buffer, in arrival order. A stream socket leaves this empty and reads
   * the ring as an undivided byte stream. */
  u32 msg_len[UNIX_MSG_SLOTS];
  u8 msg_head, msg_tail, msg_count;
  /* Monotonic counter stamped into every control block queued on this socket.
   * See struct unix_control::order. */
  u64 ctl_order;

  /* The peer called shutdown(SHUT_WR): it will send nothing more, so this
   * socket must report end-of-file once the bytes already in the ring have
   * been read. Linux's half-close, and not a nicety: `udevadm control --ping`
   * shuts its write half down and then waits for systemd-udevd to close the
   * connection, and systemd-udevd closes it when its read returns 0. With no
   * way to deliver that zero the daemon held the connection open and every
   * `udevadm control` call ran to its timeout against a daemon that had
   * already answered. */
  int peer_wr_shut;

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
  u->rb_size = unix_rb_size();
  u->rb_buffer = kmalloc(u->rb_size);
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

/*
 * The abstract namespace.
 *
 * An AF_UNIX address whose sun_path starts with a NUL byte names a socket that
 * has no filesystem entry at all: the name is the bytes after that NUL, it is
 * not NUL-terminated, and its length comes from the caller's addrlen. Linux has
 * had this since 2.2 and a great deal of software assumes it -- D-Bus offers
 * `unix:abstract=`, X11 uses it, and util-linux's agetty reaches its reload
 * socket that way, which is where this gap surfaced: every path here treated
 * sun_path as a filesystem path, so an abstract address became a lookup of the
 * empty string and every connect answered "cannot connect on UNIX socket".
 *
 * These cannot live in the VFS -- they have no name there and vanish with the
 * socket rather than with a file -- so they get their own table. The name is
 * kept with its length because it may contain NULs and is compared bytewise.
 */
#define UNIX_ABSTRACT_MAX 64
#define UNIX_ABSTRACT_NAME_MAX 107

struct unix_abstract_bind {
  struct vfs_socket_state *owner;
  u16 len; /* bytes of `name` in use, not counting the leading NUL */
  char name[UNIX_ABSTRACT_NAME_MAX];
  int used;
};

static struct unix_abstract_bind g_abstract[UNIX_ABSTRACT_MAX];
static spinlock_t g_abstract_lock = SPINLOCK_INIT;

/* An address is abstract when addrlen covers at least one byte of sun_path and
 * that byte is NUL. addrlen shorter than that is the "unnamed" form, which is
 * neither abstract nor a path. */
static int unix_addr_is_abstract(const struct b1nix_sockaddr_un *addr,
                                 usize addrlen) {
  usize hdr = sizeof(u16); /* sun_family */

  return addr && addrlen > hdr && addr->sun_path[0] == '\0';
}

static usize unix_abstract_len(usize addrlen) {
  usize hdr = sizeof(u16) + 1; /* sun_family + the leading NUL */
  usize n;

  if (addrlen <= hdr)
    return 0;
  n = addrlen - hdr;
  return n > UNIX_ABSTRACT_NAME_MAX ? UNIX_ABSTRACT_NAME_MAX : n;
}

static int unix_abstract_bind(struct vfs_socket_state *s,
                              const struct b1nix_sockaddr_un *addr,
                              usize addrlen) {
  usize n = unix_abstract_len(addrlen);
  u64 flags;
  int free_slot = -1;

  spin_lock_irqsave(&g_abstract_lock, &flags);
  for (int i = 0; i < UNIX_ABSTRACT_MAX; i++) {
    if (!g_abstract[i].used) {
      if (free_slot < 0)
        free_slot = i;
      continue;
    }
    if (g_abstract[i].len == n &&
        memcmp(g_abstract[i].name, addr->sun_path + 1, n) == 0) {
      spin_unlock_irqrestore(&g_abstract_lock, flags);
      return -EADDRINUSE;
    }
  }
  if (free_slot < 0) {
    spin_unlock_irqrestore(&g_abstract_lock, flags);
    return -ENOMEM;
  }
  g_abstract[free_slot].owner = s;
  g_abstract[free_slot].len = (u16)n;
  memcpy(g_abstract[free_slot].name, addr->sun_path + 1, n);
  g_abstract[free_slot].used = 1;
  spin_unlock_irqrestore(&g_abstract_lock, flags);
  return 0;
}

static struct vfs_socket_state *
unix_abstract_lookup(const struct b1nix_sockaddr_un *addr, usize addrlen) {
  usize n = unix_abstract_len(addrlen);
  struct vfs_socket_state *found = 0;
  u64 flags;

  spin_lock_irqsave(&g_abstract_lock, &flags);
  for (int i = 0; i < UNIX_ABSTRACT_MAX; i++) {
    if (g_abstract[i].used && g_abstract[i].len == n &&
        memcmp(g_abstract[i].name, addr->sun_path + 1, n) == 0) {
      found = g_abstract[i].owner;
      break;
    }
  }
  spin_unlock_irqrestore(&g_abstract_lock, flags);
  return found;
}

/* An abstract name lives exactly as long as the socket that bound it. */
void unix_abstract_release(struct vfs_socket_state *s) {
  u64 flags;

  spin_lock_irqsave(&g_abstract_lock, &flags);
  for (int i = 0; i < UNIX_ABSTRACT_MAX; i++) {
    if (g_abstract[i].used && g_abstract[i].owner == s) {
      g_abstract[i].used = 0;
      g_abstract[i].owner = 0;
      g_abstract[i].len = 0;
    }
  }
  spin_unlock_irqrestore(&g_abstract_lock, flags);
}

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
  ua->peer_cred = ub->owner;
  ua->has_peer_cred = 1;
  ub->peer_cred = ua->owner;
  ub->has_peer_cred = 1;
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
  unix_abstract_release(s);

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
  if (bootinfo_has_flag("b1nix.trace-exit") && u->peer) {
    console_write("UNIX-HUP: last reference to a connected socket released by pid=");
    console_write_dec(current_task ? current_task->id : 0);
    console_write("\n");
  }
  unix_lock(u);
  struct unix_socket_data *peer = u->peer;
  u->peer = 0;
  /* Snapshot still-pending backlog connectors (never accepted) to drop. */
  struct unix_socket_data *pending[UNIX_BACKLOG_MAX];
  int npending = 0;
  for (int i = 0; i < u->backlog_count && npending < UNIX_BACKLOG_MAX; i++)
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

  /* Endpoints built by connectors that were never accepted. Releasing the slot
   * reference is not enough: each one is the live peer of a connected client,
   * which would otherwise sit writing into a socket nobody will ever read.
   * Detach and wake, so the client sees the hangup — the listener going away
   * before accept is a reset, not silence. */
  for (int i = 0; i < npending; i++) {
    struct unix_socket_data *srv = pending[i];

    unix_lock(srv);
    struct unix_socket_data *cli = srv->peer;
    srv->peer = 0;
    unix_unlock(srv);
    if (cli) {
      unix_lock(cli);
      int linked = (cli->peer == srv);
      if (linked) {
        cli->peer = 0;
        if (cli->socket) cli->socket->connected = 0;
      }
      unix_unlock(cli);
      if (linked) {
        if (cli->socket) scheduler_wake_all(cli->socket);
        scheduler_wake_all(vfs_poll_chan);
        unix_data_put(srv); /* the client's reference on the endpoint */
      }
      unix_data_put(cli);   /* the endpoint's reference on the client */
    }
    unix_data_put(srv);     /* the backlog slot's reference */
  }

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
  if (u->has_peer_cred) {
    *out = u->peer_cred;
    unix_unlock(u);
    return 0;
  }
  struct unix_socket_data *peer = u->peer;
  if (!peer) {
    unix_unlock(u);
    return -ENOTCONN;
  }
  *out = peer->owner;
  unix_unlock(u);
  return 0;
}

int unix_bind(struct vfs_socket_state *s, const struct b1nix_sockaddr_un *addr,
              usize addrlen) {
  if (s->bound) return -EINVAL;

  /* An abstract name has no filesystem entry to create. */
  if (unix_addr_is_abstract(addr, addrlen)) {
    int r = unix_abstract_bind(s, addr, addrlen);
    if (r < 0)
      return r;
    s->local.un = *addr;
    s->local_un_len = addrlen;
    s->bound = 1;
    return 0;
  }

  /* Create VFS node */
  struct vfs_node *node = vfs_add_node(addr->sun_path, VFS_SOCKET, s, 0, 0);
  if (IS_ERR(node)) return (int)PTR_ERR(node);
  
  s->local.un = *addr;
  s->local_un_len = addrlen;
  s->bound = 1;
  return 0;
}

int unix_listen(struct vfs_socket_state *s, int backlog) {
  if (!s->bound || s->domain != B1NIX_AF_UNIX) return -EINVAL;
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  u->backlog_max = (backlog > UNIX_BACKLOG_MAX) ? UNIX_BACKLOG_MAX : backlog;
  if (u->backlog_max <= 0) u->backlog_max = 1;
  s->listening = 1;
  return 0;
}

int unix_connect(struct vfs_socket_state *s, const struct b1nix_sockaddr_un *addr,
                 usize addrlen, int nonblock) {
  /* Kept for the caller's shape, and deliberately unused: a local connection
   * completes within this call whether or not the socket blocks. */
  (void)nonblock;
  /* /dev/log: the kernel is the syslog sink (no userspace syslogd). musl's
   * syslog() connect()s a SOCK_DGRAM here; accept it and forward later sends to
   * the serial console instead of requiring a bound peer socket. */
  if (addr->sun_path[0] &&
      strcmp(addr->sun_path, "/dev/log") == 0) {
    s->syslog_sink = 1;
    s->connected = 1;
    return 0;
  }
  /* Which socket a client actually talks to. `systemctl` reaches the manager
   * over /run/systemd/private for some calls and over the D-Bus system bus for
   * others, and "the same socket answers other calls quickly" is a claim about
   * which path a given invocation took -- not something to assume.
   * b1nix.trace-unix-connect names it, with the caller, for every connect. */
  int trace = bootinfo_has_flag("b1nix.trace-unix-connect");
  if (trace) {
    console_write("UNIX-CONNECT: pid=");
    console_write_dec(current_task ? current_task->id : 0);
    console_write(" comm=");
    console_write(current_task && current_task->name ? current_task->name : "?");
    console_write(" path=");
    /* An abstract name is not a C string -- it starts with a NUL and its
     * length comes from addrlen -- so print the readable part after it. */
    console_write(unix_addr_is_abstract(addr, addrlen) ? "@" : "");
    console_write(unix_addr_is_abstract(addr, addrlen) ? addr->sun_path + 1
                                                       : addr->sun_path);
    console_write("\n");
  }
  /* An abstract peer is found in the table rather than the filesystem, and
   * there is no vfs node to hold a reference on -- the binding lives and dies
   * with the socket, so `peer_node` stays NULL and every put below is guarded.
   */
  struct vfs_node *peer_node = 0;
  struct vfs_socket_state *peer_s = 0;

  if (unix_addr_is_abstract(addr, addrlen)) {
    peer_s = unix_abstract_lookup(addr, addrlen);
    if (!peer_s)
      return -ECONNREFUSED;
  } else {
    peer_node = vfs_find_node(addr->sun_path);
    if (IS_ERR(peer_node)) {
      /* A path with nothing at it is ENOENT, not ECONNREFUSED.
       *
       * Linux distinguishes the two and callers act on the difference:
       * ECONNREFUSED means "the socket is there and nobody is listening",
       * which is a service that is down and may come back, while ENOENT means
       * there is no such service on this system at all. systemd's userdb
       * clients take the second as final and the first as worth waiting on. */
      if (trace) {
        console_write("UNIX-CONNECT:   -> errno ");
        console_write_dec((u64)(usize) - (isize)PTR_ERR(peer_node));
        console_write(" (nothing at that path)\n");
      }
      return (int)PTR_ERR(peer_node) == -ENOENT ? -ENOENT : -ECONNREFUSED;
    }
    /* A path that exists but is not a socket is ECONNREFUSED on Linux, which
     * is what its unix_find_other() answers for one. */
    if (peer_node->inode->type != VFS_SOCKET) {
      vfs_node_put(peer_node);
      return -ECONNREFUSED;
    }
    peer_s = (struct vfs_socket_state *)peer_node->inode->data;
    if (trace) {
      /* What was found there, and whether anyone is listening on it. A trace
       * of connects that does not say what the far end was cannot tell "the
       * service is down" from "this kernel handed the client something else". */
      console_write("UNIX-CONNECT:   node=");
      console_write(peer_node->name);
      console_write(" listening=");
      console_write_dec((u64)(peer_s && peer_s->listening));
      console_write("\n");
    }
  }
  /* The socket file outlives its socket: after the owner closes (or crashes),
   * teardown clears inode->data. Linux semantics: ECONNREFUSED, not a deref. */
  if (!peer_s) { if (peer_node) vfs_node_put(peer_node); return -ECONNREFUSED; }
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  struct unix_socket_data *peer_u = (struct unix_socket_data *)peer_s->unix_data;
  if (!peer_u) { if (peer_node) vfs_node_put(peer_node); return -ECONNREFUSED; }
  /* Hold the listener's endpoint for as long as this connect uses it.
   *
   * Finding it and using it are not one step: the checks below, the allocation
   * of the far endpoint, and the copy of the listener's credentials all happen
   * afterwards, and nothing kept it alive across them. A server closing its
   * socket in that window — swaymsg connects in a loop while sway comes and
   * goes — freed the structure under a connector that had already tested it
   * for NULL, and the credential copy read freed memory. */
  unix_data_get(peer_u);

  /* SOCK_SEQPACKET is connection-oriented: it has a listener, a backlog and an
   * accept, and differs from a stream only in that it keeps message
   * boundaries. Every other type test in this file already says so; this one
   * did not, so a seqpacket connect fell through to the datagram branch — it
   * never checked that the peer was listening, never queued an endpoint for
   * accept(), and pointed the client straight at the LISTENING socket. The
   * client's message then landed in the listener's own ring buffer, its
   * shutdown(SHUT_WR) marked the listener, and the listener reported POLLIN
   * for ever while accept() answered EAGAIN. That is `udevadm control --ping`:
   * it writes its message, waits for the daemon to close the connection, and
   * waits out its timeout against a daemon that never received one. */
  if (s->type == B1NIX_SOCK_STREAM || s->type == B1NIX_SOCK_SEQPACKET) {
    if (!peer_s->listening) { if (peer_node) vfs_node_put(peer_node); unix_data_put(peer_u); return -ECONNREFUSED; }

    /*
     * The far endpoint is built here, by the connector, and the pair is linked
     * before connect() returns. accept() then adopts the endpoint rather than
     * creating it.
     *
     * This is what makes a local connection complete the moment it is made:
     * there is no handshake to wait for, so a client may write immediately, and
     * on Linux it does. Linking at accept() instead left a window in which the
     * socket was connected and had nowhere to write — libseat sends its first
     * request straight after connecting and treats the resulting EAGAIN as
     * fatal, so no compositor could reach seatd.
     *
     * Allocated before the listener's lock is taken: nothing else may run
     * inside it, and a failure here has to be reportable without unwinding a
     * half-queued connection.
     */
    struct unix_socket_data *srv = kzalloc(sizeof(struct unix_socket_data));
    if (!srv) { if (peer_node) vfs_node_put(peer_node); unix_data_put(peer_u); return -ENOMEM; }
    srv->rb_size = unix_rb_size();
    srv->rb_buffer = kmalloc(srv->rb_size);
    if (!srv->rb_buffer) { kfree(srv); if (peer_node) vfs_node_put(peer_node); unix_data_put(peer_u); return -ENOMEM; }
    /* SO_PEERCRED on the accepted socket must report the server's identity, so
     * the endpoint inherits it from the listening socket rather than from the
     * process that happens to be connecting. */
    srv->owner = peer_u->owner;
    srv->refcount = 1; /* the backlog slot's reference */

    /* Linked before it is published. accept() takes whatever is in the backlog
     * the instant it appears, and an endpoint published first can be adopted
     * with no peer yet — the accepted socket is then permanently unconnected,
     * which is how a server came to see no credentials for a client that was
     * plainly there. */
    srv->peer = u;
    unix_data_get(u);   /* srv->peer holds a reference on the connector */
    unix_lock(u);
    u->peer = srv;
    unix_data_get(srv); /* u->peer holds a reference on the endpoint */
    u->had_peer = 1;
    srv->had_peer = 1;
    u->peer_cred = srv->owner;
    u->has_peer_cred = 1;
    srv->peer_cred = u->owner;
    srv->has_peer_cred = 1;
    s->connected = 1;
    unix_unlock(u);

    unix_lock(peer_u);
    if (peer_u->backlog_count >= peer_u->backlog_max) {
      unix_unlock(peer_u);
      /* Unlink again — the connection is refused, so neither side may keep a
       * pointer to the other. */
      unix_lock(u);
      u->peer = 0;
      s->connected = 0;
      unix_unlock(u);
      unix_data_put(srv); /* the connector's reference */
      unix_data_put(u);   /* the endpoint's reference on the connector */
      kfree(srv->rb_buffer);
      kfree(srv);
      if (peer_node) vfs_node_put(peer_node);
      unix_data_put(peer_u); return -ECONNREFUSED;
    }
    peer_u->backlog[peer_u->backlog_count++] = srv;
    unix_unlock(peer_u);

    /* Wake up peer for accept() */
    scheduler_wake_all(peer_s);
    scheduler_wake_all(vfs_poll_chan);

    /* Connected, blocking or not: nothing is pending, so there is nothing to
     * wait for. accept() on the other side turns the queued endpoint into a
     * socket; it does not decide whether this call succeeded. */
  } else {
    /* DGRAM */
    unix_data_get(peer_u); /* u->peer holds a reference on the peer */
    u->peer = peer_u;
    s->connected = 1;
    u->had_peer = 1;
  }

  if (peer_node) vfs_node_put(peer_node);
  unix_data_put(peer_u); /* the reference taken at lookup */
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
      struct unix_socket_data *srv = u->backlog[0];
      for (int i = 0; i < u->backlog_count - 1; i++) u->backlog[i] = u->backlog[i+1];
      u->backlog_count--;
      unix_unlock(u);

      /*
       * Adopt the endpoint the connector built (see unix_connect), rather than
       * linking a fresh one: it is already the peer of the connecting socket,
       * and anything written since connect() returned is already in its buffer.
       * The state the socket layer pre-allocated for this accept is discarded —
       * it has never been visible to anyone.
       *
       * The backlog slot's reference becomes the new socket's, so neither side
       * changes count here.
       */
      unix_data_put(new_u);
      new_s->unix_data = srv;
      srv->socket = new_s;
      /* An accepted socket's own address is the listener's, and callers act on
       * it: dbus asks getsockname() what family its connection is on and only
       * negotiates file-descriptor passing when the answer is AF_UNIX. With
       * the address left zeroed it read family 0, refused NEGOTIATE_UNIX_FD,
       * and sd-bus then rejected every message carrying a descriptor
       * (EOPNOTSUPP) — which is how `systemd-run --pipe` hands its stdio to
       * PID 1. */
      new_s->local.un = s->local.un;
      new_s->bound = s->bound;
      /* A connector that closed while still queued leaves the endpoint with no
       * peer: the accepted socket is then already at end of stream, which is
       * exactly what happened and what the reader should see. */
      new_s->connected = (srv->peer != 0);

      if (srv->peer && srv->peer->socket)
        scheduler_wake_all(srv->peer->socket);

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

/* SO_RCVTIMEO/SO_SNDTIMEO deadlines, in scheduler ticks (100 Hz). Shared shape
 * with the INET paths in socket.c: 0 means wait forever. */
static u64 unix_deadline(u64 timeout_ms) {
  if (!timeout_ms)
    return 0;
  u64 ticks = (timeout_ms + 9) / 10;
  return scheduler_get_ticks() + (ticks ? ticks : 1);
}

static int unix_deadline_passed(u64 deadline) {
  return deadline != 0 && scheduler_get_ticks() >= deadline;
}

static u64 unix_deadline_remaining(u64 deadline) {
  u64 now = scheduler_get_ticks();
  return deadline <= now ? 1 : deadline - now;
}

/* The socket bound at `addr`, with a reference taken, or NULL when nothing is
 * listening there. Used by an UNCONNECTED datagram send, which names its
 * destination per message instead of once at connect time. */
static struct unix_socket_data *unix_lookup_dest(
    const struct b1nix_sockaddr_un *addr) {
  if (!addr || !addr->sun_path[0])
    return 0;
  struct vfs_node *n = vfs_find_node(addr->sun_path);
  if (IS_ERR(n))
    return 0;
  if (n->inode->type != VFS_SOCKET) {
    vfs_node_put(n);
    return 0;
  }
  struct vfs_socket_state *ps = (struct vfs_socket_state *)n->inode->data;
  struct unix_socket_data *pu =
      ps ? (struct unix_socket_data *)ps->unix_data : 0;
  if (pu)
    unix_data_get(pu);
  vfs_node_put(n);
  return pu;
}

/* `dest` non-NULL: an unconnected datagram send, addressed per message.
 *
 * This is how sd_notify(3) talks to PID 1 — one AF_UNIX SOCK_DGRAM socket, no
 * connect, sendmsg with msg_name = $NOTIFY_SOCKET — so with the address thrown
 * away and the call answered ENOTCONN, no Type=notify unit could ever report
 * itself started. dbus.service is one, and systemd terminated it on its
 * start-up timeout every time. */
static isize unix_send_to(struct vfs_socket_state *s, const void *buf,
                          usize len, struct vfs_handle **handles,
                          usize nhandles, const struct b1nix_ucred *cred,
                          int nonblock, const struct b1nix_sockaddr_un *dest);

isize unix_send_control(struct vfs_socket_state *s, const void *buf, usize len,
                        struct vfs_handle **handles, usize nhandles,
                        const struct b1nix_ucred *cred, int nonblock) {
  return unix_send_to(s, buf, len, handles, nhandles, cred, nonblock, 0);
}

isize unix_sendto(struct vfs_socket_state *s,
                  const struct b1nix_sockaddr_un *addr, const void *buf,
                  usize len, int nonblock) {
  return unix_send_to(s, buf, len, 0, 0, 0, nonblock, addr);
}

static isize unix_send_to(struct vfs_socket_state *s, const void *buf,
                          usize len, struct vfs_handle **handles,
                          usize nhandles, const struct b1nix_ucred *cred,
                          int nonblock, const struct b1nix_sockaddr_un *dest) {
  u64 snd_deadline = unix_deadline(s->so_sndtimeo_ms);
  /* /dev/log syslog sink: forward the datagram to the serial console prefixed
   * with "/dev/log: " (matches the M54-LOG smoke expectation). No peer/ring. */
  /* /dev/log with nothing bound there: the kernel is the syslog sink. A
   * sendto() names the path per message, so the check has to look at the
   * destination as well as at what connect() recorded. */
  int to_syslog = s->syslog_sink;
  if (!to_syslog && dest && dest->sun_path[0] &&
      strcmp(dest->sun_path, "/dev/log") == 0) {
    struct unix_socket_data *probe = unix_lookup_dest(dest);
    if (probe)
      unix_data_put(probe);
    else
      to_syslog = 1;
  }
  if (to_syslog) {
    char line[512];
    usize n = (buf && len < sizeof(line) - 1) ? len : (buf ? sizeof(line) - 1 : 0);
    /* `buf` is already a KERNEL buffer: every caller (send, sendto, sendmsg,
     * write) bounces the payload in before reaching the socket layer. Copying
     * it in a second time as if it were a user pointer failed the user-range
     * check, so every syslog datagram — everything dbus-daemon logs, which is
     * the only place it reports why it is exiting — came back EFAULT. */
    if (n)
      memcpy(line, buf, n);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      n--;
    line[n] = '\0';
    serial_write("/dev/log: ");
    serial_write(line);
    serial_write("\n");
    return (isize)len;
  }
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;
  /* Ancillary data needs a byte to ride on only where there is no message to
   * carry it: a datagram or seqpacket socket delivers the empty message
   * itself, so ancillary data on one has somewhere to go. */
  int msg_boundaries =
      (s->type == B1NIX_SOCK_SEQPACKET || s->type == B1NIX_SOCK_DGRAM);
  if ((nhandles || cred) && len == 0 && !msg_boundaries)
    return -EINVAL;

retry:;
  /* Pin the peer for the duration of the write. Reading u->peer and taking the
   * reference under u's lock is what makes this safe: the peer closing on
   * another CPU clears u->peer and drops the link reference in unix_free_state,
   * but our own reference keeps the peer's data (and its ring buffer) alive
   * until we are done — without this the old code dereferenced freed memory. */
  struct unix_socket_data *peer_u;
  if (dest) {
    peer_u = unix_lookup_dest(dest); /* reference taken */
    if (!peer_u)
      return -ECONNREFUSED;
  } else {
    unix_lock(u);
    peer_u = u->peer;
    if (!s->connected || !peer_u) {
      unix_unlock(u);
      return -ENOTCONN;
    }
    unix_data_get(peer_u);
    unix_unlock(u);
  }

  unix_lock(peer_u);
  /* A zero-length write on a byte stream moves no bytes and is not an event:
   * Linux returns 0 without disturbing the connection. Falling through queued
   * an ancillary block for a message that would never arrive, and could block
   * on a full ring for a write that needs no room at all. */
  if (len == 0 && !msg_boundaries && !nhandles) {
    unix_unlock(peer_u);
    unix_data_put(peer_u);
    return 0;
  }
  usize free_space = peer_u->rb_size - peer_u->rb_count;
  /* A zero-length datagram needs a message slot, not room in the ring. */
  if (free_space == 0 && !(len == 0 && msg_boundaries)) {
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
    if (unix_deadline_passed(snd_deadline)) {
      unix_data_put(peer_u);
      return -EAGAIN; /* SO_SNDTIMEO expired with the buffer still full */
    }
    if (snd_deadline)
      scheduler_wait_prepare_timeout(psock, unix_deadline_remaining(snd_deadline));
    else
      scheduler_wait_prepare(psock);
    unix_lock(peer_u);
    int still_full = (peer_u->rb_count >= peer_u->rb_size);
    unix_unlock(peer_u);
    unix_data_put(peer_u);
    if (!dest && !s->connected) {   /* peer hung up → retry reports ENOTCONN */
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

  /* Attach this sender's credentials even though the sender did not ask to
   * send any. Linux's maybe_add_creds(), rule for rule: either end asking for
   * them is reason enough — and so is the receiving endpoint having NO socket
   * yet, which is the case that matters.
   *
   * An endpoint sitting in a listener's backlog has not been accepted, so it
   * has no socket and therefore no SO_PASSCRED to read. A client that writes
   * immediately after connect(2) — which is every request/response protocol
   * with a one-shot connection — would then be quoting a flag the server has
   * not had the chance to set. Linux stamps the message in that window;
   * requiring the flag meant `udevadm control` sent a message that arrived
   * with no credentials, and systemd-udevd answered "No sender credentials
   * received, ignoring message": the ping connected, was delivered, and did
   * nothing. */
  struct b1nix_ucred auto_cred;
  int cred_implicit = 0;
  if (!cred && (!peer_u->socket || peer_u->socket->so_passcred ||
                s->so_passcred)) {
    auto_cred.pid = (int)scheduler_get_pid();
    const struct cred *c = scheduler_get_current_cred();
    auto_cred.uid = c ? c->uid : 0;
    auto_cred.gid = c ? c->gid : 0;
    cred = &auto_cred;
    cred_implicit = 1;
  }

  /* SO_TIMESTAMP on the receiving end: the arrival time belongs to the moment
   * the bytes land in the receive buffer, which is here — not to whenever the
   * reader gets round to calling recvmsg. */
  u64 stamp_usec = 0;
  if (peer_u->socket &&
      (peer_u->socket->so_timestamp || peer_u->socket->so_timestampns))
    stamp_usec = unix_wallclock_usec();

  int control_slot = -1;
  if (nhandles || cred || stamp_usec) {
    for (int i = 0; i < UNIX_CONTROL_SLOTS; i++)
      if (!peer_u->control[i].used) {
        control_slot = i;
        break;
      }
    if (control_slot < 0) {
      /* Out of ancillary slots is out of room, not a broken connection.
       * Returning ENOBUFS here failed a write that had every chance of
       * succeeding a moment later, and a bus client treats a failed write as
       * a dead peer: PID 1's connection to dbus was torn down mid-call
       * ("Got disconnect on API bus") whenever more than a few messages were
       * in flight at once. Wait for the reader, exactly as a full ring does. */
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

  /* A datagram keeps its boundaries. SOCK_DGRAM used to be read as an
   * undivided byte stream, so two messages queued back to back arrived as one
   * — which is not a smaller difference for a protocol like journald's or
   * sd_notify's, it is a different protocol. */
  int seqpacket = msg_boundaries;
  if (seqpacket) {
    /* A SOCK_SEQPACKET write is all-or-nothing: a message that cannot fit
     * whole is either too large ever (EMSGSIZE) or has to wait for the reader
     * to drain, exactly as it does on Linux. */
    if (len > peer_u->rb_size) {
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
    ctl->order = ++peer_u->ctl_order;
    ctl->nhandles = nhandles;
    for (usize i = 0; i < nhandles; i++)
      ctl->handles[i] = handles[i];
    if (cred) {
      ctl->cred = *cred;
      ctl->has_cred = 1;
      ctl->cred_implicit = cred_implicit;
    }
    ctl->stamp_usec = stamp_usec;
  }
  for (usize i = 0; i < to_copy; i++) {
    peer_u->rb_buffer[peer_u->rb_tail] = ((const char *)buf)[i];
    peer_u->rb_tail = (peer_u->rb_tail + 1) % peer_u->rb_size;
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

/* Arrival stamp of the message the last recv on this socket handed over, in
 * wall-clock microseconds. 0 when nothing was stamped. */
u64 unix_last_timestamp_usec(struct vfs_socket_state *s) {
  struct unix_socket_data *u = s ? (struct unix_socket_data *)s->unix_data : 0;
  return u ? u->last_stamp_usec : 0;
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

  u64 rcv_deadline = unix_deadline(s->so_rcvtimeo_ms);

  int msg_boundaries =
      (s->type == B1NIX_SOCK_SEQPACKET || s->type == B1NIX_SOCK_DGRAM);

  while (1) {
    unix_lock(u);
    /* A queued message with no bytes in it is still a message: Linux delivers
     * a zero-length datagram, and the ancillary data attached to it is
     * sometimes the whole content. Gating readability on bytes alone made such
     * a send succeed and never arrive -- it sat in the queue with the reader
     * parked in poll. (This was written while chasing systemd-udevd's stalled
     * worker queue; that turned out to be the netlink source address instead,
     * and no zero-length AF_UNIX datagram is sent in that boot at all. The
     * behaviour is still wrong, and is still fixed here.) */
    if (u->rb_count > 0 || (msg_boundaries && u->msg_count > 0)) {
      int seqpacket = msg_boundaries;
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
        /* Several control blocks can share an offset once a zero-length
         * message is in the queue; the oldest is the one this message
         * carries. */
        if (candidate->seq == u->read_seq &&
            (!ctl || candidate->order < ctl->order))
          ctl = candidate;
      }
      for (usize i = 0; i < to_copy; i++) {
        usize idx = (u->rb_head + i) % u->rb_size;
        ((char *)buf)[i] = u->rb_buffer[idx];
      }
      usize consume = to_copy;
      if (seqpacket && !(flags & B1NIX_MSG_PEEK))
        consume = avail; /* the rest of the message is dropped, not re-read */
      if (!(flags & B1NIX_MSG_PEEK))
        u->rb_head = (u->rb_head + consume) % u->rb_size;
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
          if (ctl->has_cred && cred && has_cred &&
              (!ctl->cred_implicit || s->so_passcred)) {
            *cred = ctl->cred;
            *has_cred = 1;
          }
          u->last_stamp_usec = ctl->stamp_usec;
          memset(ctl, 0, sizeof(*ctl));
        }
      }
      unix_unlock(u);
      
      /* Wake up anyone waiting to send more data */
      scheduler_wake_all(u->socket);
      scheduler_wake_all(vfs_poll_chan);
      
      return (isize)to_copy;
    }
    int peer_done = __atomic_load_n(&u->peer_wr_shut, __ATOMIC_ACQUIRE);
    unix_unlock(u);
    /* The ring is empty and the peer has closed its write half: that is
     * end-of-file, exactly as if the peer had closed the socket. */
    if (peer_done)
      return 0;
    if ((s->type == B1NIX_SOCK_STREAM || s->type == B1NIX_SOCK_SEQPACKET) &&
        !s->connected) {
      /* End of file on a stream socket is how a child decides its parent has
       * finished with it — Chromium's zygote exits on exactly this. When one
       * arrives that nobody asked for, the reader's identity and the moment it
       * happened are the whole of the evidence. b1nix.trace-exit turns it on;
       * an EOF ends the read, so this cannot repeat in a loop. */
      if (bootinfo_has_flag("b1nix.trace-exit")) {
        console_write("UNIX-EOF: reader pid=");
        console_write_dec(current_task ? current_task->id : 0);
        console_write(" had_peer=");
        console_write_dec((u64)u->had_peer);
        console_write("\n");
      }
      return 0;
    }
    if (flags & B1NIX_MSG_DONTWAIT) return -EAGAIN;
    /* SMP-safe wait: publish BLOCKED on our socket, then re-test under the lock
     * so a sender's wake (scheduler_wake_all(u->socket) after a write/close)
     * racing between the check above and the block cannot be lost. */
    if (unix_deadline_passed(rcv_deadline))
      return -EAGAIN; /* SO_RCVTIMEO expired with nothing received */
    if (rcv_deadline)
      scheduler_wait_prepare_timeout(s, unix_deadline_remaining(rcv_deadline));
    else
      scheduler_wait_prepare(s);
    unix_lock(u);
    int have_data =
        (u->rb_count > 0 || (msg_boundaries && u->msg_count > 0));
    int disconnected =
        ((s->type == B1NIX_SOCK_STREAM || s->type == B1NIX_SOCK_SEQPACKET) &&
         !s->connected) ||
        __atomic_load_n(&u->peer_wr_shut, __ATOMIC_ACQUIRE);
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
  /* Bytes, or a message that has none (see unix_recv_control). */
  if (u->rb_count > 0 ||
      ((s->type == B1NIX_SOCK_SEQPACKET || s->type == B1NIX_SOCK_DGRAM) &&
       u->msg_count > 0))
    pfd->revents |= B1NIX_POLLIN;
  /* POLLHUP means a peer that WAS there is gone. SOCK_SEQPACKET is
   * connection-oriented too, and its hangup is the whole protocol for
   * `udevadm control`: the client sends its message and then waits for
   * systemd-udevd to CLOSE the accepted connection. Reporting no hangup on a
   * seqpacket left `udevadm control --ping` and `udevadm settle` blocked until
   * their timeout, on a daemon that had already answered.
   *
   * A listening socket has no
   * peer by definition and a fresh socket has not had one yet; reporting HUP
   * for either tells an event loop that its socket died, which is how sway's
   * IPC listener ended up unusable — every dispatch saw HANGUP on the one fd
   * it was waiting to accept connections on. */
  if ((s->type == B1NIX_SOCK_STREAM || s->type == B1NIX_SOCK_SEQPACKET) &&
      !s->listening && !s->connected && u->had_peer)
    pfd->revents |= B1NIX_POLLHUP;
  
  /* Check if peer has space for writing. Pin the peer while we read it so a
   * concurrent close cannot free it under us. */
  if (s->connected) {
    unix_lock(u);
    struct unix_socket_data *peer_u = u->peer;
    if (peer_u) unix_data_get(peer_u);
    unix_unlock(u);
    if (peer_u) {
      if (peer_u->rb_count < peer_u->rb_size) pfd->revents |= B1NIX_POLLOUT;
      unix_data_put(peer_u);
    }
  } else if (s->type == B1NIX_SOCK_DGRAM) {
      pfd->revents |= B1NIX_POLLOUT;
  }
  
  if (s->listening && u->backlog_count > 0) pfd->revents |= B1NIX_POLLIN;

  /* A peer that has shut its write half down has made this socket readable:
   * the read that follows returns 0. An event loop that is never told stays in
   * epoll_wait for ever holding a connection whose other end is finished with
   * it. Linux reports EPOLLRDHUP alongside, for a reader that asked. */
  if (__atomic_load_n(&u->peer_wr_shut, __ATOMIC_ACQUIRE)) {
    pfd->revents |= B1NIX_POLLIN;
    if (pfd->events & B1NIX_POLLRDHUP)
      pfd->revents |= B1NIX_POLLRDHUP;
  }

  return 0;
}

/* shutdown(2) on an AF_UNIX socket. Closing the write half is a statement the
 * PEER has to hear -- it is the only way a reader learns that no more data is
 * coming without the descriptor being closed -- so the flag is set on the peer
 * and the peer is woken. */
int unix_shutdown(struct vfs_socket_state *s, int how_wr, int how_rd) {
  struct unix_socket_data *u = (struct unix_socket_data *)s->unix_data;

  (void)how_rd;
  if (!u || !how_wr)
    return 0;
  unix_lock(u);
  struct unix_socket_data *peer = u->peer;
  if (peer)
    unix_data_get(peer);
  unix_unlock(u);
  if (!peer)
    return 0;
  __atomic_store_n(&peer->peer_wr_shut, 1, __ATOMIC_RELEASE);
  if (peer->socket)
    scheduler_wake_all(peer->socket);
  scheduler_wake_all(vfs_poll_chan);
  unix_data_put(peer);
  return 0;
}
