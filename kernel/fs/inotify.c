/*
 * M73: inotify — filesystem change notification.
 *
 * An inotify fd (VFS_HANDLE_INOTIFY) hangs a struct inotify_instance off
 * handle->private_data: a small watch table (path-node -> mask -> wd) and a ring
 * of pending events. VFS mutation sites call vfs_inotify_notify(), which scans
 * every active instance for a watch on the affected node and enqueues a matching
 * event, waking pollers. read() drains the ring as Linux struct inotify_event
 * records; the fd is pollable. The common no-watch path is a single atomic load.
 */

#include <b1nix/errno.h>
#include <b1nix/inotify.h>
#include <b1nix/mm.h>
#include <b1nix/posix.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <string.h>

#define INOTIFY_MAX_INSTANCES 16
#define INOTIFY_MAX_WATCHES 32
#define INOTIFY_QUEUE 64
#define INOTIFY_NAME_MAX 255

struct inotify_watch {
  int wd;                /* watch descriptor (>0), 0 = free slot */
  struct vfs_node *node; /* watched node (refcounted) */
  u32 mask;
};

struct inotify_event_rec {
  int wd;
  u32 mask;
  u32 cookie;
  u32 len;                       /* name field length incl. NUL+padding */
  char name[INOTIFY_NAME_MAX + 1];
};

struct inotify_instance {
  struct inotify_watch watches[INOTIFY_MAX_WATCHES];
  struct inotify_event_rec ev[INOTIFY_QUEUE];
  int head, tail, count; /* event ring */
  int next_wd;
  spinlock_t lock;
};

/* Registry of live instances so vfs_inotify_notify can find watchers. */
static struct inotify_instance *g_instances[INOTIFY_MAX_INSTANCES];
static spinlock_t g_reg_lock = SPINLOCK_INIT;
static int g_inotify_active; /* count of live instances — hot-path gate */

static void inotify_release_by_ptr(struct inotify_instance *in);

/* ── event ring ─────────────────────────────────────────────────────────── */

/* Enqueue an event into `in` (caller holds in->lock). Drops to IN_Q_OVERFLOW if
 * the ring is full (Linux behaviour). */
static void inotify_enqueue(struct inotify_instance *in, int wd, u32 mask,
                            const char *name) {
  if (in->count >= INOTIFY_QUEUE) {
    /* overwrite the tail-most with an overflow marker once */
    struct inotify_event_rec *o = &in->ev[(in->head + in->count - 1) % INOTIFY_QUEUE];
    o->wd = -1;
    o->mask = IN_Q_OVERFLOW;
    o->len = 0;
    o->name[0] = 0;
    return;
  }
  struct inotify_event_rec *r = &in->ev[in->tail];
  r->wd = wd;
  r->mask = mask;
  r->cookie = 0;
  if (name && name[0]) {
    usize nl = strlen(name);
    if (nl > INOTIFY_NAME_MAX)
      nl = INOTIFY_NAME_MAX;
    memcpy(r->name, name, nl);
    r->name[nl] = 0;
    /* Linux pads the name to a multiple of the event struct alignment (4/8);
     * round up to 8 incl. the NUL. */
    r->len = (u32)((nl + 1 + 7) & ~7u);
  } else {
    r->name[0] = 0;
    r->len = 0;
  }
  in->tail = (in->tail + 1) % INOTIFY_QUEUE;
  in->count++;
}

/* ── fd file-ops ────────────────────────────────────────────────────────── */

static isize inotify_read(struct vfs_handle *h, char *buf, usize len) {
  struct inotify_instance *in = (struct inotify_instance *)h->private_data;
  if (!in)
    return -EINVAL;
  if (len < sizeof(struct inotify_event))
    return -EINVAL;

  for (;;) {
    usize produced = 0;
    u64 flags;
    spin_lock_irqsave(&in->lock, &flags);
    while (in->count > 0) {
      struct inotify_event_rec *r = &in->ev[in->head];
      usize need = sizeof(struct inotify_event) + r->len;
      if (produced + need > len)
        break; /* no room for this record */
      struct inotify_event *ie = (struct inotify_event *)(buf + produced);
      ie->wd = r->wd;
      ie->mask = r->mask;
      ie->cookie = r->cookie;
      ie->len = r->len;
      if (r->len) {
        memset(ie->name, 0, r->len);
        memcpy(ie->name, r->name, strlen(r->name));
      }
      produced += need;
      in->head = (in->head + 1) % INOTIFY_QUEUE;
      in->count--;
    }
    spin_unlock_irqrestore(&in->lock, flags);
    if (produced > 0)
      return (isize)produced;
    /* Empty. POSIX: read of fewer than one event with a pending big event would
     * EINVAL, but with an empty queue we block (or EAGAIN). */
    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    scheduler_block_on_timeout(vfs_poll_chan, 1);
  }
}

static int inotify_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct inotify_instance *in = (struct inotify_instance *)h->private_data;
  pfd->revents = 0;
  if (in && __atomic_load_n(&in->count, __ATOMIC_ACQUIRE) > 0)
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

static void inotify_release(struct vfs_handle *h) {
  struct inotify_instance *in = (struct inotify_instance *)h->private_data;
  if (!in)
    return;
  h->private_data = 0;
  /* Unregister and drop watch node refs. */
  u64 flags;
  spin_lock_irqsave(&g_reg_lock, &flags);
  for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
    if (g_instances[i] == in) {
      g_instances[i] = 0;
      __atomic_fetch_sub(&g_inotify_active, 1, __ATOMIC_RELEASE);
      break;
    }
  }
  spin_unlock_irqrestore(&g_reg_lock, flags);
  for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
    if (in->watches[i].wd && in->watches[i].node)
      vfs_node_put(in->watches[i].node);
  }
  kfree(in);
}

static const struct vfs_file_ops inotify_ops = {
    .read = inotify_read,
    .poll = inotify_poll,
    .release = inotify_release,
};

/* ── syscall backends ───────────────────────────────────────────────────── */

int vfs_inotify_init1(int flags) {
  if (flags & ~(IN_CLOEXEC | IN_NONBLOCK))
    return -EINVAL;

  struct inotify_instance *in = kzalloc(sizeof(*in));
  if (!in)
    return -ENOMEM;
  in->lock = SPINLOCK_INIT;
  in->next_wd = 1;

  /* Register. */
  u64 rf;
  spin_lock_irqsave(&g_reg_lock, &rf);
  int slot = -1;
  for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++)
    if (!g_instances[i]) { slot = i; break; }
  if (slot < 0) {
    spin_unlock_irqrestore(&g_reg_lock, rf);
    kfree(in);
    return -ENFILE;
  }
  g_instances[slot] = in;
  __atomic_fetch_add(&g_inotify_active, 1, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&g_reg_lock, rf);

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_INOTIFY);
  if (!h) {
    inotify_release_by_ptr(in); /* fwd-declared below */
    return -ENFILE;
  }
  h->private_data = in;
  h->ops = &inotify_ops;
  h->flags = (flags & IN_NONBLOCK) ? B1NIX_O_NONBLOCK : 0;
  h->flags |= B1NIX_O_RDONLY;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h); /* triggers inotify_release */
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  if (flags & IN_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

/* Helper used only on the alloc_raw_handle failure path (no handle to release). */
static void inotify_release_by_ptr(struct inotify_instance *in) {
  u64 flags;
  spin_lock_irqsave(&g_reg_lock, &flags);
  for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++)
    if (g_instances[i] == in) {
      g_instances[i] = 0;
      __atomic_fetch_sub(&g_inotify_active, 1, __ATOMIC_RELEASE);
      break;
    }
  spin_unlock_irqrestore(&g_reg_lock, flags);
  kfree(in);
}

int vfs_inotify_add_watch(int fd, const char *user_path, u32 mask) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_INOTIFY)
    return -EINVAL;
  struct inotify_instance *in = (struct inotify_instance *)h->private_data;
  if (!in)
    return -EINVAL;
  if ((mask & IN_ALL_EVENTS) == 0)
    return -EINVAL;

  char kpath[VFS_MAX_PATH];
  if (syscall_copyinstr(kpath, sizeof(kpath), user_path) < 0)
    return -EFAULT;
  char resolved[VFS_MAX_PATH];
  vfs_resolve_path(kpath, resolved);
  struct vfs_node *node = vfs_find_node(resolved); /* refcounted */
  if (!node)
    return -ENOENT;

  u64 flags;
  spin_lock_irqsave(&in->lock, &flags);
  /* Existing watch on this node? Update the mask. */
  for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
    if (in->watches[i].wd && in->watches[i].node == node) {
      in->watches[i].mask = (mask & 0x80000000u /* IN_MASK_ADD */)
                                ? (in->watches[i].mask | mask)
                                : mask;
      int wd = in->watches[i].wd;
      spin_unlock_irqrestore(&in->lock, flags);
      vfs_node_put(node); /* already hold a ref in the existing watch */
      return wd;
    }
  }
  /* New watch. */
  for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
    if (in->watches[i].wd == 0) {
      in->watches[i].wd = in->next_wd++;
      in->watches[i].node = node; /* keep the ref */
      in->watches[i].mask = mask & IN_ALL_EVENTS;
      int wd = in->watches[i].wd;
      spin_unlock_irqrestore(&in->lock, flags);
      return wd;
    }
  }
  spin_unlock_irqrestore(&in->lock, flags);
  vfs_node_put(node);
  return -ENOSPC;
}

int vfs_inotify_rm_watch(int fd, int wd) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_INOTIFY)
    return -EINVAL;
  struct inotify_instance *in = (struct inotify_instance *)h->private_data;
  if (!in)
    return -EINVAL;
  u64 flags;
  struct vfs_node *to_put = 0;
  spin_lock_irqsave(&in->lock, &flags);
  for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
    if (in->watches[i].wd == wd) {
      to_put = in->watches[i].node;
      in->watches[i].wd = 0;
      in->watches[i].node = 0;
      in->watches[i].mask = 0;
      /* IN_IGNORED tells userspace the watch was removed. */
      inotify_enqueue(in, wd, IN_IGNORED, 0);
      spin_unlock_irqrestore(&in->lock, flags);
      if (to_put)
        vfs_node_put(to_put);
      scheduler_wake_all(in);
      scheduler_wake_all(vfs_poll_chan);
      return 0;
    }
  }
  spin_unlock_irqrestore(&in->lock, flags);
  return -EINVAL;
}

/* ── VFS hook ───────────────────────────────────────────────────────────── */

void vfs_inotify_notify(struct vfs_node *node, u32 mask, const char *name) {
  if (!node || __atomic_load_n(&g_inotify_active, __ATOMIC_ACQUIRE) == 0)
    return;
  int woke = 0;
  u64 rf;
  spin_lock_irqsave(&g_reg_lock, &rf);
  for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
    struct inotify_instance *in = g_instances[i];
    if (!in)
      continue;
    u64 lf;
    spin_lock_irqsave(&in->lock, &lf);
    for (int w = 0; w < INOTIFY_MAX_WATCHES; w++) {
      if (in->watches[w].wd && in->watches[w].node == node &&
          (in->watches[w].mask & mask)) {
        /* Report the matched event bits plus the IN_ISDIR qualifier, which
         * Linux preserves even though it is not part of the watch mask. */
        u32 report = (mask & in->watches[w].mask) | (mask & IN_ISDIR);
        inotify_enqueue(in, in->watches[w].wd, report, name);
        woke = 1;
      }
    }
    spin_unlock_irqrestore(&in->lock, lf);
  }
  spin_unlock_irqrestore(&g_reg_lock, rf);
  /* Readers block on the global vfs_poll_chan; a single wake after dropping all
   * locks suffices and keeps the scheduler runqueue lock out from under
   * g_reg_lock. */
  if (woke)
    scheduler_wake_all(vfs_poll_chan);
}
