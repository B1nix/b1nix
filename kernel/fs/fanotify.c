/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fanotify(7). See <b1nix/fanotify.h> for what it is for.
 *
 * Three things distinguish it from inotify, and all three are here:
 *
 *   A mark can cover a whole MOUNT, not one directory. That is what lets a
 *   scanner watch a filesystem without walking it first.
 *
 *   An event carries an open DESCRIPTOR for the object, not a name relative to
 *   a watch. The descriptor is created in the reader's own table, at read(2)
 *   time, because that is the only moment the kernel knows who is reading.
 *
 *   A PERMISSION event stops the access until the monitor answers. The thread
 *   that opened the file sleeps inside the open, the monitor writes FAN_ALLOW
 *   or FAN_DENY, and open(2) either continues or returns EPERM. A monitor that
 *   goes away while an access waits does not hang the machine: its descriptor
 *   closing releases every waiter with an allow, because a scanner that has
 *   crashed must not become a lock on the filesystem.
 */
#include <b1nix/fanotify.h>

#include <b1nix/errno.h>
#include <b1nix/inotify.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/vfs.h>

#include <string.h>

/* fanotify_init(2)/fanotify_mark(2): 300/301 on x86_64, 262/263 on aarch64. */
#if defined(__aarch64__)
#define FAN_NR_init 262
#define FAN_NR_mark 263
#else
#define FAN_NR_init 300
#define FAN_NR_mark 301
#endif

#define FAN_MAX_GROUPS 16
#define FAN_MAX_MARKS 64
#define FAN_MAX_EVENTS 128
/* How long an access waits for a monitor's verdict before it is allowed
 * through. Linux waits for ever and relies on the monitor being correct; a
 * scanner that stops answering would otherwise freeze every open on the
 * machine, and this kernel prefers a filesystem that keeps working. */
#define FAN_PERM_TIMEOUT_TICKS 5000

extern void *vfs_poll_chan;

struct fan_mark {
  struct vfs_node *node; /* the object, or the mount root for a mount mark */
  u64 mask;
  int mount;  /* the mark covers everything under that root */
  int used;
};

struct fan_event {
  u64 mask;
  struct vfs_node *node;
  usize pid;
  /* Permission events only: the verdict, and what identifies the answer. */
  int perm;
  volatile int answered;
  volatile int allow;
  i32 perm_fd; /* the descriptor the monitor was given, which its answer names */
};

struct fan_group {
  u32 flags;
  u32 event_f_flags;
  int nonblock;
  int dead;
  int refs;

  struct fan_mark marks[FAN_MAX_MARKS];

  struct fan_event events[FAN_MAX_EVENTS];
  u32 head, tail;

  spinlock_t lock;
  char perm_chan;
};

static struct fan_group *g_groups[FAN_MAX_GROUPS];
static spinlock_t g_fan_lock = SPINLOCK_INIT;
static int g_fan_active; /* live groups: the hot-path gate */

int fanotify_active(void) {
  return __atomic_load_n(&g_fan_active, __ATOMIC_RELAXED) != 0;
}

static void fan_put(struct fan_group *g) {
  if (!g)
    return;
  if (__atomic_sub_fetch(&g->refs, 1, __ATOMIC_ACQ_REL) == 0)
    kfree(g);
}

/* ── translating the VFS's event bits ────────────────────────────────────── */

/* The hook sites speak inotify's bit space, which is the one every existing
 * caller already passes. */
static u64 fan_from_inotify(u32 in_mask) {
  u64 m = 0;

  if (in_mask & IN_ACCESS)
    m |= FAN_ACCESS;
  if (in_mask & IN_MODIFY)
    m |= FAN_MODIFY;
  if (in_mask & IN_ATTRIB)
    m |= FAN_ATTRIB;
  if (in_mask & IN_CLOSE_WRITE)
    m |= FAN_CLOSE_WRITE;
  if (in_mask & IN_CLOSE_NOWRITE)
    m |= FAN_CLOSE_NOWRITE;
  if (in_mask & IN_OPEN)
    m |= FAN_OPEN;
  if (in_mask & IN_CREATE)
    m |= FAN_CREATE;
  if (in_mask & IN_DELETE)
    m |= FAN_DELETE;
  if (in_mask & IN_DELETE_SELF)
    m |= FAN_DELETE_SELF;
  if (in_mask & IN_MOVED_FROM)
    m |= FAN_MOVED_FROM;
  if (in_mask & IN_MOVED_TO)
    m |= FAN_MOVED_TO;
  return m;
}

/* Is `node` inside the subtree rooted at `root`? A mount mark watches
 * everything below its root, so this walks parents rather than comparing
 * pointers. */
static int fan_under(struct vfs_node *node, struct vfs_node *root) {
  int depth = 0;

  for (struct vfs_node *n = node; n && depth < 128; n = n->parent, depth++) {
    if (n == root)
      return 1;
  }
  return 0;
}

/* The mask a group wants for this node, 0 when it is not watching it. */
static u64 fan_group_mask(struct fan_group *g, struct vfs_node *node) {
  u64 m = 0;

  for (int i = 0; i < FAN_MAX_MARKS; i++) {
    if (!g->marks[i].used)
      continue;
    if (g->marks[i].mount) {
      if (fan_under(node, g->marks[i].node))
        m |= g->marks[i].mask;
    } else if (g->marks[i].node == node) {
      m |= g->marks[i].mask;
    }
  }
  return m;
}

/* ── the queue ───────────────────────────────────────────────────────────── */

/* Caller holds the group lock. Returns the slot, or 0 when the queue is
 * full -- Linux reports FAN_Q_OVERFLOW then, and so does the reader here. */
static struct fan_event *fan_enqueue(struct fan_group *g, u64 mask,
                                     struct vfs_node *node, int perm) {
  u32 next = (g->tail + 1) % FAN_MAX_EVENTS;

  if (next == g->head) {
    /* Mark the newest event as an overflow rather than losing the fact. */
    u32 last = (g->tail + FAN_MAX_EVENTS - 1) % FAN_MAX_EVENTS;

    g->events[last].mask |= FAN_Q_OVERFLOW;
    return 0;
  }

  struct fan_event *e = &g->events[g->tail];

  memset(e, 0, sizeof(*e));
  e->mask = mask;
  e->node = node;
  e->pid = current_task ? current_task->id : 0;
  e->perm = perm;
  e->perm_fd = -1;
  if (node)
    vfs_node_get(node);
  g->tail = next;
  return e;
}

/* ── the notification entry points ───────────────────────────────────────── */

static int fan_deliver(struct vfs_node *node, u32 in_mask, int permission) {
  if (!node || !fanotify_active())
    return 0;

  u64 want = fan_from_inotify(in_mask);

  if (permission) {
    /* A permission event is a different bit: a monitor that asked only for
     * FAN_OPEN is told about the open but has no veto. */
    if (want & FAN_OPEN)
      want = FAN_OPEN_PERM;
    else if (want & FAN_ACCESS)
      want = FAN_ACCESS_PERM;
    else
      return 0;
  }
  if (!want)
    return 0;

  int verdict = 0;
  u64 rf;

  spin_lock_irqsave(&g_fan_lock, &rf);
  for (int i = 0; i < FAN_MAX_GROUPS; i++) {
    struct fan_group *g = g_groups[i];

    if (!g || g->dead)
      continue;

    u64 lf;

    spin_lock_irqsave(&g->lock, &lf);
    u64 mask = fan_group_mask(g, node) & want;
    struct fan_event *e = mask ? fan_enqueue(g, mask, node, permission) : 0;

    spin_unlock_irqrestore(&g->lock, lf);
    if (!e)
      continue;
    if (!permission)
      continue;

    /* A permission event: hold this reference while we wait, so the group
     * cannot be freed under us if the monitor closes its descriptor. */
    __atomic_add_fetch(&g->refs, 1, __ATOMIC_ACQ_REL);
    spin_unlock_irqrestore(&g_fan_lock, rf);
    scheduler_wake_all(vfs_poll_chan);

    for (int spin = 0; spin < FAN_PERM_TIMEOUT_TICKS; spin++) {
      if (e->answered || g->dead)
        break;
      if (current_task && current_task->pending_signals)
        break;
      scheduler_wait_prepare_timeout(&g->perm_chan, 1);
      if (e->answered || g->dead)
        scheduler_wait_cancel();
      else
        scheduler_wait_commit();
    }
    if (e->answered && !e->allow)
      verdict = -EPERM;
    fan_put(g);
    spin_lock_irqsave(&g_fan_lock, &rf);
    if (verdict)
      break;
  }
  spin_unlock_irqrestore(&g_fan_lock, rf);
  scheduler_wake_all(vfs_poll_chan);
  return verdict;
}

int fanotify_notify(struct vfs_node *node, u32 in_mask) {
  return fan_deliver(node, in_mask, 0);
}

int fanotify_permission(struct vfs_node *node, u32 in_mask) {
  return fan_deliver(node, in_mask, 1);
}

/* ── the descriptor ──────────────────────────────────────────────────────── */

/* Hand the reader an open descriptor for the object the event names. Linux
 * does exactly this, and it is why an event costs a file descriptor. */
static i32 fan_make_fd(struct vfs_node *node) {
  struct vfs_handle *h;
  int fd;

  if (!node)
    return -1;
  h = alloc_raw_handle(VFS_HANDLE_NODE);
  if (!h)
    return -1;
  vfs_node_get(node);
  h->node = node;
  h->ops = &node_file_ops;
  h->flags = B1NIX_O_RDONLY;
  h->no_notify = 1; /* see struct vfs_handle: this must not feed itself */
  fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_node_put(node);
    vfs_handle_release(h);
    return -1;
  }
  return (i32)fd;
}

static isize fan_read(struct vfs_handle *h, char *buf, usize len) {
  struct fan_group *g = h ? (struct fan_group *)h->private_data : 0;
  usize written = 0;

  if (!g)
    return -EBADF;
  if (len < sizeof(struct fanotify_event_metadata))
    return -EINVAL;

  for (;;) {
    u64 flags;
    int have;

    spin_lock_irqsave(&g->lock, &flags);
    have = g->head != g->tail;
    spin_unlock_irqrestore(&g->lock, flags);
    if (have)
      break;
    if (g->dead)
      return 0;
    if (g->nonblock || (h->flags & B1NIX_O_NONBLOCK))
      return -EAGAIN;
    if (current_task && current_task->pending_signals)
      return -EINTR;
    scheduler_wait_prepare_timeout(vfs_poll_chan, 10);
    spin_lock_irqsave(&g->lock, &flags);
    have = g->head != g->tail;
    spin_unlock_irqrestore(&g->lock, flags);
    if (have)
      scheduler_wait_cancel();
    else
      scheduler_wait_commit();
  }

  while (written + sizeof(struct fanotify_event_metadata) <= len) {
    u64 flags;
    struct fan_event *e = 0;
    struct vfs_node *node = 0;
    u64 mask = 0;
    usize pid = 0;
    int perm = 0;
    u32 slot = 0;

    spin_lock_irqsave(&g->lock, &flags);
    if (g->head != g->tail) {
      slot = g->head;
      e = &g->events[slot];
      node = e->node;
      mask = e->mask;
      pid = e->pid;
      perm = e->perm;
      /* A permission event stays in the array until it is answered -- the
       * writer finds it by the descriptor number below -- but it leaves the
       * queue so it is read once. */
      g->head = (g->head + 1) % FAN_MAX_EVENTS;
    }
    spin_unlock_irqrestore(&g->lock, flags);
    if (!e)
      break;

    struct fanotify_event_metadata md;

    memset(&md, 0, sizeof(md));
    md.event_len = sizeof(md);
    md.vers = FANOTIFY_METADATA_VERSION;
    md.metadata_len = sizeof(md);
    md.mask = mask;
    md.pid = (i32)pid;
    md.fd = fan_make_fd(node);
    if (perm)
      e->perm_fd = md.fd;
    memcpy(buf + written, &md, sizeof(md));
    written += sizeof(md);

    if (node && !perm)
      vfs_node_put(node); /* the queue's reference; the fd has its own */
    if (!perm)
      e->node = 0;
  }
  return (isize)written;
}

/* write(2) on the group is how a monitor answers a permission event. */
static isize fan_write(struct vfs_handle *h, const char *buf, usize len) {
  struct fan_group *g = h ? (struct fan_group *)h->private_data : 0;
  struct fanotify_response resp;

  if (!g)
    return -EBADF;
  if (len < sizeof(resp))
    return -EINVAL;
  memcpy(&resp, buf, sizeof(resp));
  if (resp.response != FAN_ALLOW && resp.response != FAN_DENY)
    return -EINVAL;

  u64 flags;
  int found = 0;

  spin_lock_irqsave(&g->lock, &flags);
  for (int i = 0; i < FAN_MAX_EVENTS; i++) {
    struct fan_event *e = &g->events[i];

    if (e->perm && !e->answered && e->perm_fd == resp.fd) {
      e->allow = (resp.response == FAN_ALLOW);
      e->answered = 1;
      if (e->node) {
        vfs_node_put(e->node);
        e->node = 0;
      }
      found = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&g->lock, flags);
  scheduler_wake_all(&g->perm_chan);
  return found ? (isize)sizeof(resp) : -ENOENT;
}

static int fan_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct fan_group *g = h ? (struct fan_group *)h->private_data : 0;
  u64 flags;

  pfd->revents = 0;
  if (!g)
    return -EBADF;
  spin_lock_irqsave(&g->lock, &flags);
  if (g->head != g->tail)
    pfd->revents |= B1NIX_POLLIN;
  spin_unlock_irqrestore(&g->lock, flags);
  pfd->revents |= B1NIX_POLLOUT; /* a response can always be written */
  return 0;
}

static void fan_release(struct vfs_handle *h) {
  struct fan_group *g = h ? (struct fan_group *)h->private_data : 0;
  u64 flags;

  if (!g)
    return;
  h->private_data = 0;
  g->dead = 1;

  spin_lock_irqsave(&g_fan_lock, &flags);
  for (int i = 0; i < FAN_MAX_GROUPS; i++)
    if (g_groups[i] == g)
      g_groups[i] = 0;
  __atomic_fetch_sub(&g_fan_active, 1, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&g_fan_lock, flags);

  /* Anything waiting for a verdict is allowed through: a monitor that has gone
   * must not be a lock on the filesystem. */
  spin_lock_irqsave(&g->lock, &flags);
  for (int i = 0; i < FAN_MAX_EVENTS; i++) {
    if (g->events[i].perm && !g->events[i].answered) {
      g->events[i].allow = 1;
      g->events[i].answered = 1;
    }
    if (g->events[i].node) {
      vfs_node_put(g->events[i].node);
      g->events[i].node = 0;
    }
  }
  for (int i = 0; i < FAN_MAX_MARKS; i++) {
    if (g->marks[i].used && g->marks[i].node) {
      vfs_node_put(g->marks[i].node);
      g->marks[i].used = 0;
      g->marks[i].node = 0;
    }
  }
  spin_unlock_irqrestore(&g->lock, flags);
  scheduler_wake_all(&g->perm_chan);
  scheduler_wake_all(vfs_poll_chan);

  if (h->node) {
    vfs_node_put(h->node);
    h->node = 0;
  }
  fan_put(g);
}

static const struct vfs_file_ops fan_file_ops = {
    .read = fan_read,
    .write = fan_write,
    .poll = fan_poll,
    .release = fan_release,
};

/* ── the system calls ────────────────────────────────────────────────────── */

static isize fan_init(u64 flags, u64 event_f_flags) {
  const struct cred *cred = scheduler_get_current_cred();

  /* fanotify sees every process's file access, so it is privileged -- Linux
   * requires CAP_SYS_ADMIN for the same reason. */
  if (!cred || !cred_has_cap_effective(cred, CAP_SYS_ADMIN))
    return -EPERM;
  if (flags & ~(u64)(FAN_CLOEXEC | FAN_NONBLOCK | FAN_CLASS_CONTENT |
                     FAN_CLASS_PRE_CONTENT | FAN_UNLIMITED_QUEUE |
                     FAN_UNLIMITED_MARKS | FAN_REPORT_TID))
    return -EINVAL;

  struct fan_group *g = kzalloc(sizeof(*g));

  if (!g)
    return -ENOMEM;
  g->flags = (u32)flags;
  g->event_f_flags = (u32)event_f_flags;
  g->nonblock = (flags & FAN_NONBLOCK) != 0;
  g->refs = 1;
  g->lock = SPINLOCK_INIT;

  struct vfs_node *node = vfs_create_node(VFS_DEVICE);

  if (!node) {
    kfree(g);
    return -ENOMEM;
  }
  strncpy(node->name, "fanotify", sizeof(node->name) - 1);
  node->name[sizeof(node->name) - 1] = 0;
  node->deleted = 1;
  node->inode->nlink = 0;
  node->inode->mode = 0600;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_NODE);

  if (!h) {
    vfs_node_put(node);
    kfree(g);
    return -ENFILE;
  }
  h->node = node;
  h->private_data = g;
  h->ops = &fan_file_ops;
  h->flags = B1NIX_O_RDWR | (g->nonblock ? B1NIX_O_NONBLOCK : 0);

  u64 lf;
  int slot = -1;

  spin_lock_irqsave(&g_fan_lock, &lf);
  for (int i = 0; i < FAN_MAX_GROUPS; i++) {
    if (!g_groups[i]) {
      g_groups[i] = g;
      slot = i;
      break;
    }
  }
  if (slot >= 0)
    __atomic_fetch_add(&g_fan_active, 1, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&g_fan_lock, lf);
  if (slot < 0) {
    vfs_handle_release(h);
    vfs_node_put(node);
    kfree(g);
    return -EMFILE;
  }

  int fd = scheduler_fd_alloc(h);

  if (fd < 0) {
    fan_release(h);
    return fd;
  }
  if (flags & FAN_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

static isize fan_mark(int fanfd, u64 flags, u64 mask, int dirfd,
                      u64 upath) {
  struct vfs_handle *h = scheduler_fd_get(fanfd);

  if (!h || h->ops != &fan_file_ops)
    return -EINVAL;

  struct fan_group *g = (struct fan_group *)h->private_data;

  if (!g)
    return -EINVAL;
  if (!(flags & (FAN_MARK_ADD | FAN_MARK_REMOVE | FAN_MARK_FLUSH)))
    return -EINVAL;

  u64 lf;

  if (flags & FAN_MARK_FLUSH) {
    spin_lock_irqsave(&g->lock, &lf);
    for (int i = 0; i < FAN_MAX_MARKS; i++) {
      if (g->marks[i].used && g->marks[i].node)
        vfs_node_put(g->marks[i].node);
      g->marks[i].used = 0;
      g->marks[i].node = 0;
    }
    spin_unlock_irqrestore(&g->lock, lf);
    return 0;
  }

  char path[VFS_MAX_PATH];
  char resolved[VFS_MAX_PATH];
  struct vfs_node *node = 0;

  if (upath) {
    if (syscall_copyinstr(path, sizeof(path), (const char *)(usize)upath) < 0)
      return -EFAULT;
    if (path[0] == '/') {
      node = vfs_find_node(path);
    } else if (dirfd == AT_FDCWD) {
      vfs_resolve_path(path, resolved);
      node = vfs_find_node(resolved);
    } else {
      /* Relative to a directory descriptor. The descriptor's own node is the
       * base, and only a single component is joined to it -- enough for what
       * a monitor marks, and refusing anything else beats resolving it
       * wrongly. */
      struct vfs_handle *dh = scheduler_fd_get(dirfd);

      if (!dh || !dh->node)
        return -EBADF;
      node = find_child(dh->node, path);
      if (node)
        vfs_node_get(node);
    }
  } else {
    /* A mark with no path names the directory descriptor itself. */
    struct vfs_handle *dh = scheduler_fd_get(dirfd);

    if (dh && dh->node) {
      node = dh->node;
      vfs_node_get(node);
    }
  }
  if (!node || IS_ERR(node))
    return -ENOENT;

  /* A mount or filesystem mark watches the whole subtree below the object,
   * which is the point of fanotify: no walk, no watch per directory. */
  int is_mount = (flags & (FAN_MARK_MOUNT | FAN_MARK_FILESYSTEM)) != 0;
  int rc = -ENOENT;

  spin_lock_irqsave(&g->lock, &lf);
  for (int i = 0; i < FAN_MAX_MARKS; i++) {
    if (g->marks[i].used && g->marks[i].node == node &&
        g->marks[i].mount == is_mount) {
      if (flags & FAN_MARK_REMOVE) {
        g->marks[i].mask &= ~mask;
        if (!g->marks[i].mask) {
          vfs_node_put(g->marks[i].node);
          g->marks[i].used = 0;
          g->marks[i].node = 0;
        }
      } else {
        g->marks[i].mask |= mask;
      }
      rc = 0;
      goto out;
    }
  }
  if (flags & FAN_MARK_REMOVE)
    goto out; /* nothing of ours to remove */
  for (int i = 0; i < FAN_MAX_MARKS; i++) {
    if (!g->marks[i].used) {
      g->marks[i].node = node;
      g->marks[i].mask = mask;
      g->marks[i].mount = is_mount;
      g->marks[i].used = 1;
      vfs_node_get(node);
      rc = 0;
      goto out;
    }
  }
  rc = -ENOSPC;
out:
  spin_unlock_irqrestore(&g->lock, lf);
  vfs_node_put(node);
  return rc;
}

int fanotify_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 *ret) {
  if (nr == FAN_NR_init) {
    *ret = (u64)fan_init(a0, a1);
    return 1;
  }
  if (nr == FAN_NR_mark) {
    *ret = (u64)fan_mark((int)a0, a1, a2, (int)a3, a4);
    return 1;
  }
  return 0;
}
