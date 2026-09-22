/* SPDX-License-Identifier: GPL-2.0-only */
/* POSIX message queues (M123) — mq_open(3) and friends, and the "mqueue"
 * filesystem they live in.
 *
 * Every IPC namespace has its own directory of queues. mq_open() names a queue
 * in the caller's namespace and returns a descriptor on that file; mounting
 * "mqueue" exposes the same directory, so `ls /dev/mqueue` lists the queues,
 * reading one reports its state and `rm` removes it, as on Linux.
 *
 * A queue holds messages in priority order (highest first, FIFO within one
 * priority). Senders block while it is full and receivers while it is empty,
 * unless the descriptor is O_NONBLOCK; an absolute CLOCK_REALTIME timeout bounds
 * either wait. mq_notify() registers one process to be told, by signal, that a
 * message arrived on an empty queue nobody was waiting on.
 */

#include <b1nix/errno.h>
#include <b1nix/ktime.h>
#include <b1nix/mm.h>
#include <b1nix/mqueue.h>
#include <b1nix/namespace.h>
#include <b1nix/netlink.h>
#include <b1nix/posix.h>
#include <b1nix/rtc.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/uidgid.h>
#include <b1nix/user_namespace.h>
#include <b1nix/vfs.h>
#include <stdio.h>
#include <string.h>

#define MQ_NAME_MAX 255

/* struct mq_attr, Linux layout. */
struct lx_mq_attr {
  i64 mq_flags;
  i64 mq_maxmsg;
  i64 mq_msgsize;
  i64 mq_curmsgs;
  i64 __reserved[4];
};

/* struct sigevent, Linux layout (64 bytes). */
struct lx_sigevent {
  union sigval sigev_value;
  i32 sigev_signo;
  i32 sigev_notify;
  i32 pad[12];
};

#define SIGEV_SIGNAL 0
#define SIGEV_NONE   1
#define SIGEV_THREAD 2
#define SI_MESGQ     (-3)

/* SIGEV_THREAD: libc parks a thread on a netlink socket and registers a cookie
 * with it; the kernel sends the cookie back with its last byte saying why. */
#define NOTIFY_COOKIE_LEN 32
#define NOTIFY_WOKENUP    1
#define NOTIFY_REMOVED    2

struct mq_msg {
  struct mq_msg *next;
  u32 prio;
  usize len;
  char data[];
};

struct mqueue {
  u32 ns;
  i64 maxmsg;
  i64 msgsize;
  i64 curmsgs;
  u64 qbytes;
  struct mq_msg *head; /* priority order */
  usize receivers;     /* tasks blocked in mq_timedreceive */
  /* mq_notify registration: the process to signal, or 0. */
  usize notify_pid;
  int notify_signo;
  int notify_type;
  union sigval notify_value;
  struct vfs_handle *notify_sock; /* SIGEV_THREAD, retained */
  u8 notify_cookie[NOTIFY_COOKIE_LEN];
};

/* A notification taken out of a queue under the lock, delivered after it. */
struct mq_pending_notify {
  usize pid;
  int signo;
  int type;
  union sigval value;
  struct vfs_handle *sock;
  u8 cookie[NOTIFY_COOKIE_LEN];
};

static void mq_notify_deliver(struct mq_pending_notify *n, u8 why) {
  if (n->sock) {
    n->cookie[NOTIFY_COOKIE_LEN - 1] = why;
    (void)netlink_kernel_unicast(n->sock, n->cookie, NOTIFY_COOKIE_LEN);
    vfs_handle_release(n->sock);
    n->sock = 0;
    return;
  }
  if (!n->pid || n->type != SIGEV_SIGNAL)
    return;
  if (n->signo >= 32)
    scheduler_sigqueue(n->pid, n->signo, n->value, SI_MESGQ);
  else
    scheduler_kill(n->pid, n->signo);
}

/* Take the registration out of `q`. Caller holds mq_lock. */
static void mq_notify_take_locked(struct mqueue *q,
                                  struct mq_pending_notify *out) {
  memset(out, 0, sizeof(*out));
  out->pid = q->notify_pid;
  out->signo = q->notify_signo;
  out->type = q->notify_type;
  out->value = q->notify_value;
  out->sock = q->notify_sock;
  memcpy(out->cookie, q->notify_cookie, NOTIFY_COOKIE_LEN);
  q->notify_pid = 0;
  q->notify_sock = 0;
}

/* One lock for every queue: operations are short list updates. */
static spinlock_t mq_lock = SPINLOCK_INIT;
static struct vfs_node *mq_roots[NS_MAX_IPC];
static u32 mq_count[NS_MAX_IPC];

static void mq_node_release(struct vfs_node *node);

static struct mqueue *mq_of_node(struct vfs_node *node) {
  if (!node || !node->inode || node->inode->release_cb != mq_node_release)
    return 0;
  return (struct mqueue *)node->inode->data;
}

/* The queue behind a descriptor, with the handle retained (release it). */
static struct mqueue *mq_of_fd(int fd, struct vfs_handle **out) {
  struct vfs_handle *h = vfs_handle_acquire(fd);
  if (!h)
    return 0;
  struct mqueue *q =
      h->kind == VFS_HANDLE_NODE ? mq_of_node(h->node) : 0;
  if (!q) {
    vfs_handle_release(h);
    return 0;
  }
  *out = h;
  return q;
}

static void mq_free_messages(struct mq_msg *m) {
  while (m) {
    struct mq_msg *next = m->next;
    kfree(m);
    m = next;
  }
}

static void mq_node_release(struct vfs_node *node) {
  struct mqueue *q = (struct mqueue *)node->inode->data;
  node->inode->data = 0;
  if (!q)
    return;
  u64 f;
  spin_lock_irqsave(&mq_lock, &f);
  if (q->ns < NS_MAX_IPC && mq_count[q->ns])
    mq_count[q->ns]--;
  struct mq_msg *msgs = q->head;
  q->head = 0;
  struct mq_pending_notify n;
  mq_notify_take_locked(q, &n);
  spin_unlock_irqrestore(&mq_lock, f);
  if (n.sock)
    mq_notify_deliver(&n, NOTIFY_REMOVED);
  mq_free_messages(msgs);
  kfree(q);
}

/* "QSIZE:..." — what cat /dev/mqueue/<name> shows. */
static isize mq_read_cb(struct vfs_node *node, u64 offset, char *buf,
                        usize size, int flags) {
  (void)flags;
  struct mqueue *q = mq_of_node(node);
  if (!q)
    return -EBADF;
  char tmp[128];
  u64 f;
  spin_lock_irqsave(&mq_lock, &f);
  u64 qsize = q->qbytes;
  int signo = q->notify_type == SIGEV_SIGNAL ? q->notify_signo : 0;
  int notify = q->notify_pid ? q->notify_type : 0;
  usize npid = q->notify_pid;
  spin_unlock_irqrestore(&mq_lock, f);
  int n = snprintf(tmp, sizeof(tmp),
                   "QSIZE:%-10llu NOTIFY:%-5d SIGNO:%-5d NOTIFY_PID:%-6llu\n",
                   (unsigned long long)qsize, notify, signo,
                   (unsigned long long)namespace_pid_to_user(npid));
  if (n < 0 || offset >= (u64)n)
    return 0;
  usize len = (usize)n - (usize)offset;
  if (len > size)
    len = size;
  memcpy(buf, tmp + offset, len);
  return (isize)len;
}

static int mq_poll_cb(struct vfs_handle *h, struct vfs_node *node,
                      struct b1nix_pollfd *pfd) {
  (void)h;
  struct mqueue *q = mq_of_node(node);
  pfd->revents = 0;
  if (!q)
    return 0;
  u64 f;
  spin_lock_irqsave(&mq_lock, &f);
  if (q->curmsgs > 0)
    pfd->revents |= B1NIX_POLLIN;
  if (q->curmsgs < q->maxmsg)
    pfd->revents |= B1NIX_POLLOUT;
  spin_unlock_irqrestore(&mq_lock, f);
  return 0;
}

static int mq_dir_unlink_cb(struct vfs_node *dir, const char *name) {
  (void)dir;
  (void)name;
  return 0; /* the VFS drops the node; the queue dies with its last user */
}

/* ── the per-namespace directory ─────────────────────────────────────────── */

static struct vfs_node *mq_root_of(u32 ns) {
  if (ns >= NS_MAX_IPC)
    return 0;
  u64 f;
  spin_lock_irqsave(&mq_lock, &f);
  struct vfs_node *root = mq_roots[ns];
  spin_unlock_irqrestore(&mq_lock, f);
  if (root)
    return root;
  struct vfs_node *n = vfs_create_node(VFS_DIRECTORY);
  if (!n)
    return 0;
  n->inode->mode = 01777;
  n->inode->uid = 0;
  n->inode->gid = 0;
  n->inode->nlink = 2;
  n->inode->unlink_cb = mq_dir_unlink_cb;
  spin_lock_irqsave(&mq_lock, &f);
  if (!mq_roots[ns]) {
    mq_roots[ns] = n;
    n = 0;
  }
  root = mq_roots[ns];
  spin_unlock_irqrestore(&mq_lock, f);
  if (n) { /* lost the race */
    n->deleted = 1;
    vfs_node_put(n);
  }
  return root;
}

static struct vfs_node *mq_find(struct vfs_node *root, const char *name) {
  for (struct vfs_node *c = root->first_child; c; c = c->next_sibling)
    if (!c->deleted && strcmp(c->name, name) == 0)
      return c;
  return 0;
}

static int mq_check_name(const char *name) {
  usize len = strlen(name);
  if (len == 0)
    return -ENOENT;
  if (len > MQ_NAME_MAX || len >= sizeof(((struct vfs_node *)0)->name))
    return -ENAMETOOLONG;
  for (usize i = 0; i < len; i++)
    if (name[i] == '/')
      return -EACCES;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return -EINVAL;
  return 0;
}

/* May the caller open a queue file for `acc` (0400 read, 0200 write)? */
static int mq_permission(struct vfs_node *node, u16 acc) {
  const struct cred *c = scheduler_get_current_cred();
  if (!c)
    return 0;
  u16 mode = node->inode->mode;
  u16 granted;
  if (c->fsuid == node->inode->uid)
    granted = (u16)((mode >> 6) & 7);
  else if (c->fsgid == node->inode->gid)
    granted = (u16)((mode >> 3) & 7);
  else {
    granted = (u16)(mode & 7);
    for (int i = 0; i < c->ngroups && i < MAX_GROUPS; i++)
      if (c->groups[i] == node->inode->gid)
        granted = (u16)((mode >> 3) & 7);
  }
  u16 want = (u16)(acc >> 6);
  if ((want & ~granted) == 0)
    return 0;
  if (capable_wrt_inode_uidgid(c, node->inode->uid, node->inode->gid,
                               CAP_DAC_OVERRIDE))
    return 0;
  return -EACCES;
}

int mqueue_open(const char *name, int oflag, u32 mode, const void *user_attr) {
  int rc = mq_check_name(name);
  if (rc < 0)
    return rc;
  u32 ns = namespace_current_id(NS_IPC);
  struct vfs_node *root = mq_root_of(ns);
  if (!root)
    return -ENOMEM;
  const struct cred *c = scheduler_get_current_cred();

  struct lx_mq_attr attr;
  int have_attr = 0;
  if ((oflag & B1NIX_O_CREAT) && user_attr) {
    if (syscall_copyin(&attr, user_attr, sizeof(attr)) < 0)
      return -EFAULT;
    have_attr = 1;
  }
  int accmode = oflag & 3;
  if (accmode == 3)
    return -EINVAL;
  u16 acc = accmode == B1NIX_O_RDONLY   ? 0400
            : accmode == B1NIX_O_WRONLY ? 0200
                                        : 0600;

  /* A new queue is prepared outside the lock; it is only published if the
   * name is still free. */
  struct mqueue *nq = 0;
  struct vfs_node *nn = 0;
  if (oflag & B1NIX_O_CREAT) {
    i64 maxmsg = MQ_MSG_DEFAULT, msgsize = MQ_MSGSIZE_DEFAULT;
    if (have_attr) {
      if (attr.mq_maxmsg <= 0 || attr.mq_msgsize <= 0)
        return -EINVAL;
      int priv = c && ns_capable_cred(c, namespace_owner(NS_IPC, ns),
                                      CAP_SYS_RESOURCE);
      if (attr.mq_maxmsg > (priv ? MQ_MSG_HARDMAX : MQ_MSG_MAX) ||
          attr.mq_msgsize > (priv ? MQ_MSGSIZE_HARDMAX : MQ_MSGSIZE_MAX))
        return -EINVAL;
      maxmsg = attr.mq_maxmsg;
      msgsize = attr.mq_msgsize;
    }
    nq = kzalloc(sizeof(*nq));
    nn = vfs_create_node(VFS_FILE);
    if (!nq || !nn) {
      kfree(nq);
      if (nn) {
        nn->deleted = 1;
        vfs_node_put(nn);
      }
      return -ENOMEM;
    }
    nq->ns = ns;
    nq->maxmsg = maxmsg;
    nq->msgsize = msgsize;
    usize nl = strlen(name);
    memcpy(nn->name, name, nl + 1);
    nn->inode->mode = (u16)(mode & 0777 & ~(c ? c->umask : 0022));
    nn->inode->uid = c ? c->fsuid : 0;
    nn->inode->gid = c ? c->fsgid : 0;
    nn->inode->read_cb = mq_read_cb;
    nn->inode->poll_cb = mq_poll_cb;
    nn->inode->release_cb = mq_node_release;
    nn->inode->data = nq;
    nn->inode->flags |= VFS_NODE_PSEUDO_REG;
  }

  struct vfs_node *node = 0;
  u64 f;
  spin_lock_irqsave(&mq_lock, &f);
  struct vfs_node *existing = mq_find(root, name);
  if (existing) {
    if ((oflag & B1NIX_O_CREAT) && (oflag & B1NIX_O_EXCL))
      rc = -EEXIST;
    else
      node = vfs_node_get(existing);
  } else if (!(oflag & B1NIX_O_CREAT)) {
    rc = -ENOENT;
  } else if (mq_count[ns] >= MQ_QUEUES_MAX) {
    rc = -ENOSPC;
  } else {
    mq_count[ns]++;
    nn->parent = root;
    nn->refcount++; /* the directory's reference */
    vfs_attach_child(root, nn);
    node = nn;
    nn = 0;
    nq = 0;
  }
  spin_unlock_irqrestore(&mq_lock, f);

  if (nn) { /* not published: the name existed or an error */
    nn->inode->data = 0;
    nn->deleted = 1;
    vfs_node_put(nn);
    kfree(nq);
  }
  if (rc < 0)
    return rc;
  /* Permission on an existing queue; a queue the caller just created is
   * opened with the access it asked for, whatever the mode it gave. */
  if (existing && mq_permission(node, acc) != 0) {
    vfs_node_put(node);
    return -EACCES;
  }
  int fd = vfs_fd_for_node(
      node, oflag & (3 | B1NIX_O_NONBLOCK));
  vfs_node_put(node);
  if (fd >= 0 && (oflag & B1NIX_O_CLOEXEC))
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

int mqueue_unlink(const char *name) {
  int rc = mq_check_name(name);
  if (rc < 0)
    return rc;
  u32 ns = namespace_current_id(NS_IPC);
  struct vfs_node *root = mq_root_of(ns);
  if (!root)
    return -ENOENT;
  const struct cred *c = scheduler_get_current_cred();
  u64 f;
  spin_lock_irqsave(&mq_lock, &f);
  struct vfs_node *node = mq_find(root, name);
  if (!node) {
    spin_unlock_irqrestore(&mq_lock, f);
    return -ENOENT;
  }
  /* The directory is sticky: only the queue's owner (or CAP_FOWNER over it)
   * removes it. */
  if (c && !cred_inode_owner_or_capable(c, node->inode->uid,
                                        node->inode->gid)) {
    spin_unlock_irqrestore(&mq_lock, f);
    return -EACCES;
  }
  vfs_detach_child(root, node);
  node->deleted = 1;
  node->inode->nlink = 0;
  spin_unlock_irqrestore(&mq_lock, f);
  vfs_node_put(node); /* the directory's reference */
  return 0;
}

/* Remaining ticks until an absolute CLOCK_REALTIME timespec, 0 for "already
 * passed", or -errno for a malformed one. */
static i64 mq_timeout_ticks(const void *user_timeout) {
  struct {
    i64 tv_sec;
    i64 tv_nsec;
  } ts;
  if (syscall_copyin(&ts, user_timeout, sizeof(ts)) < 0)
    return -EFAULT;
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
    return -EINVAL;
  u64 deadline = (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
  u64 now = rtc_now_unix_nanos();
  if (deadline <= now)
    return 0;
  u64 hz = sched_tick_hz();
  u64 ns = deadline - now;
  return (i64)((ns * hz + 999999999ULL) / 1000000000ULL);
}

/* A message arrived on an empty queue: the registered process is told, once,
 * unless a receiver was already waiting for it. Caller holds mq_lock. */
static int mq_notify_fire_locked(struct mqueue *q,
                                 struct mq_pending_notify *out) {
  if (!q->notify_pid || q->receivers)
    return 0;
  mq_notify_take_locked(q, out);
  return 1;
}

isize mqueue_timedsend(int fd, const void *user_msg, usize len, u32 prio,
                       const void *user_timeout) {
  struct vfs_handle *h;
  struct mqueue *q = mq_of_fd(fd, &h);
  if (!q)
    return -EBADF;
  isize rc = 0;
  if ((h->flags & 3) == B1NIX_O_RDONLY) {
    rc = -EBADF;
    goto out;
  }
  if (prio >= MQ_PRIO_MAX) {
    rc = -EINVAL;
    goto out;
  }
  if ((i64)len > q->msgsize) {
    rc = -EMSGSIZE;
    goto out;
  }
  struct mq_msg *m = kmalloc(sizeof(*m) + (len ? len : 1));
  if (!m) {
    rc = -ENOMEM;
    goto out;
  }
  m->next = 0;
  m->prio = prio;
  m->len = len;
  if (len && syscall_copyin(m->data, user_msg, len) < 0) {
    kfree(m);
    rc = -EFAULT;
    goto out;
  }

  for (;;) {
    u64 f;
    spin_lock_irqsave(&mq_lock, &f);
    if (q->curmsgs < q->maxmsg) {
      struct mq_msg **pp = &q->head;
      while (*pp && (*pp)->prio >= prio)
        pp = &(*pp)->next;
      m->next = *pp;
      *pp = m;
      int was_empty = q->curmsgs == 0;
      q->curmsgs++;
      q->qbytes += len;
      struct mq_pending_notify n;
      int fire = was_empty && mq_notify_fire_locked(q, &n);
      spin_unlock_irqrestore(&mq_lock, f);
      scheduler_wake_all(q);
      scheduler_wake_all(vfs_poll_chan);
      if (fire)
        mq_notify_deliver(&n, NOTIFY_WOKENUP);
      rc = 0;
      break;
    }
    if (h->flags & B1NIX_O_NONBLOCK) {
      spin_unlock_irqrestore(&mq_lock, f);
      kfree(m);
      rc = -EAGAIN;
      break;
    }
    i64 ticks = 0;
    if (user_timeout) {
      spin_unlock_irqrestore(&mq_lock, f);
      ticks = mq_timeout_ticks(user_timeout);
      if (ticks <= 0) {
        kfree(m);
        rc = ticks < 0 ? ticks : -ETIMEDOUT;
        break;
      }
      spin_lock_irqsave(&mq_lock, &f);
      if (q->curmsgs < q->maxmsg) {
        spin_unlock_irqrestore(&mq_lock, f);
        continue;
      }
    }
    if (scheduler_signal_pending()) {
      spin_unlock_irqrestore(&mq_lock, f);
      kfree(m);
      rc = -EINTR;
      break;
    }
    scheduler_wait_prepare_timeout(q, (u64)ticks);
    spin_unlock_irqrestore(&mq_lock, f);
    scheduler_wait_commit();
  }
out:
  vfs_handle_release(h);
  return rc;
}

isize mqueue_timedreceive(int fd, void *user_msg, usize len, void *user_prio,
                          const void *user_timeout) {
  struct vfs_handle *h;
  struct mqueue *q = mq_of_fd(fd, &h);
  if (!q)
    return -EBADF;
  isize rc;
  if ((h->flags & 3) == B1NIX_O_WRONLY) {
    rc = -EBADF;
    goto out;
  }
  if ((i64)len < q->msgsize) {
    rc = -EMSGSIZE;
    goto out;
  }
  for (;;) {
    u64 f;
    spin_lock_irqsave(&mq_lock, &f);
    if (q->head) {
      struct mq_msg *m = q->head;
      q->head = m->next;
      q->curmsgs--;
      q->qbytes -= m->len;
      spin_unlock_irqrestore(&mq_lock, f);
      scheduler_wake_all(q);
      scheduler_wake_all(vfs_poll_chan);
      rc = (isize)m->len;
      if (m->len && syscall_copyout(user_msg, m->data, m->len) < 0)
        rc = -EFAULT;
      else if (user_prio &&
               syscall_copyout(user_prio, &m->prio, sizeof(m->prio)) < 0)
        rc = -EFAULT;
      kfree(m);
      break;
    }
    if (h->flags & B1NIX_O_NONBLOCK) {
      spin_unlock_irqrestore(&mq_lock, f);
      rc = -EAGAIN;
      break;
    }
    i64 ticks = 0;
    if (user_timeout) {
      spin_unlock_irqrestore(&mq_lock, f);
      ticks = mq_timeout_ticks(user_timeout);
      if (ticks <= 0) {
        rc = ticks < 0 ? ticks : -ETIMEDOUT;
        break;
      }
      spin_lock_irqsave(&mq_lock, &f);
      if (q->head) {
        spin_unlock_irqrestore(&mq_lock, f);
        continue;
      }
    }
    if (scheduler_signal_pending()) {
      spin_unlock_irqrestore(&mq_lock, f);
      rc = -EINTR;
      break;
    }
    q->receivers++;
    scheduler_wait_prepare_timeout(q, (u64)ticks);
    spin_unlock_irqrestore(&mq_lock, f);
    scheduler_wait_commit();
    spin_lock_irqsave(&mq_lock, &f);
    q->receivers--;
    spin_unlock_irqrestore(&mq_lock, f);
  }
out:
  vfs_handle_release(h);
  return rc;
}

int mqueue_notify(int fd, const void *user_sigevent) {
  struct vfs_handle *h;
  struct mqueue *q = mq_of_fd(fd, &h);
  if (!q)
    return -EBADF;
  struct lx_sigevent ev;
  struct vfs_handle *sock = 0;
  u8 cookie[NOTIFY_COOKIE_LEN];
  int rc = 0;
  if (user_sigevent) {
    if (syscall_copyin(&ev, user_sigevent, sizeof(ev)) < 0) {
      rc = -EFAULT;
      goto out;
    }
    if (ev.sigev_notify == SIGEV_SIGNAL) {
      if (ev.sigev_signo <= 0 || ev.sigev_signo > NSIG_MAX) {
        rc = -EINVAL;
        goto out;
      }
    } else if (ev.sigev_notify == SIGEV_THREAD) {
      /* sigev_signo is the netlink socket, sival_ptr the cookie. */
      if (syscall_copyin(cookie, ev.sigev_value.sival_ptr, sizeof(cookie)) <
          0) {
        rc = -EFAULT;
        goto out;
      }
      sock = vfs_handle_acquire(ev.sigev_signo);
      if (!sock || sock->kind != VFS_HANDLE_SOCKET || !sock->private_data ||
          ((struct vfs_socket_state *)sock->private_data)->domain !=
              B1NIX_AF_NETLINK) {
        if (sock)
          vfs_handle_release(sock);
        rc = -EBADF;
        goto out;
      }
    } else if (ev.sigev_notify != SIGEV_NONE) {
      rc = -EINVAL;
      goto out;
    }
  }
  usize me = scheduler_get_pid();
  struct mq_pending_notify removed;
  memset(&removed, 0, sizeof(removed));
  u64 f;
  spin_lock_irqsave(&mq_lock, &f);
  if (!user_sigevent) {
    if (q->notify_pid == me)
      mq_notify_take_locked(q, &removed);
  } else if (q->notify_pid) {
    rc = -EBUSY;
  } else {
    q->notify_pid = me;
    q->notify_type = ev.sigev_notify;
    q->notify_signo = ev.sigev_signo;
    q->notify_value = ev.sigev_value;
    q->notify_sock = sock;
    if (sock)
      memcpy(q->notify_cookie, cookie, sizeof(cookie));
    sock = 0;
  }
  spin_unlock_irqrestore(&mq_lock, f);
  if (removed.sock)
    mq_notify_deliver(&removed, NOTIFY_REMOVED);
  if (sock)
    vfs_handle_release(sock);
out:
  vfs_handle_release(h);
  return rc;
}

int mqueue_getsetattr(int fd, const void *user_new, void *user_old) {
  struct vfs_handle *h;
  struct mqueue *q = mq_of_fd(fd, &h);
  if (!q)
    return -EBADF;
  int rc = 0;
  struct lx_mq_attr nattr;
  if (user_new && syscall_copyin(&nattr, user_new, sizeof(nattr)) < 0) {
    rc = -EFAULT;
    goto out;
  }
  if (user_new && (nattr.mq_flags & ~(i64)04000 /* O_NONBLOCK */)) {
    rc = -EINVAL;
    goto out;
  }
  struct lx_mq_attr old;
  memset(&old, 0, sizeof(old));
  u64 f;
  spin_lock_irqsave(&mq_lock, &f);
  old.mq_flags = (h->flags & B1NIX_O_NONBLOCK) ? 04000 : 0;
  old.mq_maxmsg = q->maxmsg;
  old.mq_msgsize = q->msgsize;
  old.mq_curmsgs = q->curmsgs;
  spin_unlock_irqrestore(&mq_lock, f);
  if (user_new) {
    if (nattr.mq_flags & 04000)
      h->flags |= B1NIX_O_NONBLOCK;
    else
      h->flags &= ~B1NIX_O_NONBLOCK;
  }
  if (user_old && syscall_copyout(user_old, &old, sizeof(old)) < 0)
    rc = -EFAULT;
out:
  vfs_handle_release(h);
  return rc;
}

void mqueue_ns_destroy(u32 ns) {
  if (ns >= NS_MAX_IPC)
    return;
  u64 f;
  spin_lock_irqsave(&mq_lock, &f);
  struct vfs_node *root = mq_roots[ns];
  mq_roots[ns] = 0;
  spin_unlock_irqrestore(&mq_lock, f);
  if (!root)
    return;
  for (;;) {
    spin_lock_irqsave(&mq_lock, &f);
    struct vfs_node *c = root->first_child;
    if (c) {
      vfs_detach_child(root, c);
      c->deleted = 1;
      c->inode->nlink = 0;
    }
    spin_unlock_irqrestore(&mq_lock, f);
    if (!c)
      break;
    vfs_node_put(c);
  }
  root->deleted = 1;
  vfs_node_put(root);
}

/* ── the filesystem ─────────────────────────────────────────────────────── */

static struct vfs_node *mq_mount_cb(const char *source, u64 flags,
                                    void *data) {
  (void)source;
  (void)flags;
  (void)data;
  struct vfs_node *root = mq_root_of(namespace_current_id(NS_IPC));
  if (!root)
    return ERR_PTR(-ENOMEM);
  return vfs_node_get(root);
}

static int mq_umount_cb(struct vfs_node *root_node) {
  (void)root_node;
  return 0; /* the queues belong to the namespace, not to the mount */
}

static struct vfs_fs mqueue_fs = {
    .name = "mqueue", .mount = mq_mount_cb, .umount = mq_umount_cb,
    .flags = VFS_FS_NODEV | VFS_FS_USERNS_MOUNT};

void mqueue_init(void) { vfs_register_fs(&mqueue_fs); }
