/* M56 — Event-loop and IPC primitives: eventfd, timerfd, signalfd and epoll,
 * plus memfd file sealing.
 *
 * All four objects are anonymous, pollable file descriptors. They follow the
 * pipe model (kernel/fs/pipe.c): allocate a raw handle of a dedicated kind,
 * hang a kmalloc'd state struct off handle->private_data, and install a
 * per-kind struct vfs_file_ops with read/write/poll/release/ioctl. Readiness
 * changes wake any poller blocked on vfs_poll_chan, so the objects work with
 * poll(2), select(2) and the epoll added here.
 *
 * The kernel timer tick (100 Hz) drives timerfd: scheduler_on_timer_tick calls
 * eventpoll_timer_tick(), which — only when at least one timerfd is armed —
 * wakes vfs_poll_chan so blocked pollers re-scan and observe expirations. The
 * expiration count itself is computed lazily from the monotonic tick on each
 * read/poll, so no per-tick per-fd bookkeeping is needed. */

#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/arch.h>
#include <b1nix/spinlock.h>
#include <b1nix/posix.h>
#include <b1nix/linux_abi.h>
#include <b1nix/user.h>
#include <stdlib.h>
#include <string.h>

/* Ticks are 100 Hz (10 ms), matching the poll/select millisecond conversion
 * elsewhere in the kernel. */
#define TICKS_PER_SEC 100ULL

extern int syscall_copyin(void *dst, const void *user_src, usize size);
extern int syscall_copyout(void *user_dst, const void *src, usize size);

/* ---- eventfd ----------------------------------------------------------- */

#define EVENTFD_MAX 0xfffffffffffffffeULL /* UINT64_MAX - 1 */

struct eventfd_state {
  volatile int lock;
  u64 count;
  int semaphore; /* EFD_SEMAPHORE: read decrements by 1 */
};

static isize eventfd_read(struct vfs_handle *h, char *buf, usize len) {
  struct eventfd_state *e = (struct eventfd_state *)h->private_data;
  if (!e || len < sizeof(u64))
    return -EINVAL;
  while (1) {
    while (__atomic_test_and_set(&e->lock, __ATOMIC_ACQUIRE))
      scheduler_yield();
    if (e->count != 0)
      break;
    __atomic_clear(&e->lock, __ATOMIC_RELEASE);
    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    scheduler_block_on(e);
  }
  u64 out;
  if (e->semaphore) {
    out = 1;
    e->count -= 1;
  } else {
    out = e->count;
    e->count = 0;
  }
  __atomic_clear(&e->lock, __ATOMIC_RELEASE);
  scheduler_wake_all(e);
  scheduler_wake_all(vfs_poll_chan);
  memcpy(buf, &out, sizeof(u64));
  return (isize)sizeof(u64);
}

static isize eventfd_write(struct vfs_handle *h, const char *buf, usize len) {
  struct eventfd_state *e = (struct eventfd_state *)h->private_data;
  if (!e || len < sizeof(u64))
    return -EINVAL;
  u64 add;
  memcpy(&add, buf, sizeof(u64));
  if (add == 0xffffffffffffffffULL)
    return -EINVAL; /* the all-ones value is reserved by the ABI */
  while (1) {
    while (__atomic_test_and_set(&e->lock, __ATOMIC_ACQUIRE))
      scheduler_yield();
    if (e->count + add <= EVENTFD_MAX) {
      e->count += add;
      __atomic_clear(&e->lock, __ATOMIC_RELEASE);
      break;
    }
    __atomic_clear(&e->lock, __ATOMIC_RELEASE);
    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    scheduler_block_on(e);
  }
  scheduler_wake_all(e);
  scheduler_wake_all(vfs_poll_chan);
  return (isize)sizeof(u64);
}

static int eventfd_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct eventfd_state *e = (struct eventfd_state *)h->private_data;
  pfd->revents = 0;
  if (!e)
    return 0;
  if (e->count > 0)
    pfd->revents |= B1NIX_POLLIN;
  if (e->count < EVENTFD_MAX)
    pfd->revents |= B1NIX_POLLOUT;
  return 0;
}

static void eventfd_release(struct vfs_handle *h) {
  if (h->private_data) {
    kfree(h->private_data);
    h->private_data = 0;
  }
}

static const struct vfs_file_ops eventfd_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .poll = eventfd_poll,
    .release = eventfd_release,
};

int vfs_eventfd(unsigned int initval, int flags) {
  if (flags & ~(B1NIX_EFD_SEMAPHORE | B1NIX_EFD_CLOEXEC | B1NIX_EFD_NONBLOCK))
    return -EINVAL;
  struct eventfd_state *e = kzalloc(sizeof(*e));
  if (!e)
    return -ENOMEM;
  e->count = initval;
  e->semaphore = (flags & B1NIX_EFD_SEMAPHORE) ? 1 : 0;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_EVENTFD);
  if (!h) {
    kfree(e);
    return -ENFILE;
  }
  h->private_data = e;
  h->ops = &eventfd_ops;
  h->flags = (flags & B1NIX_EFD_NONBLOCK) ? B1NIX_O_NONBLOCK : 0;
  h->flags |= B1NIX_O_RDWR;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  if (flags & B1NIX_EFD_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

/* ---- timerfd ----------------------------------------------------------- */

/* Count of currently-armed timerfds. The timer ISR wakes pollers only when
 * this is non-zero (the common no-timer case is a single relaxed load). */
static volatile int g_armed_timerfds = 0;

struct timerfd_state {
  volatile int lock;
  int armed;
  u64 next_tick;     /* absolute tick of the next expiration; 0 = disarmed */
  u64 interval_ticks; /* 0 = one-shot */
  u64 expirations;   /* accumulated, cleared on read */
};

/* Compute and fold in any expirations that have elapsed since the last update.
 * Caller holds t->lock. */
static void timerfd_advance(struct timerfd_state *t) {
  if (!t->armed || t->next_tick == 0)
    return;
  u64 now = scheduler_get_uptime_ticks();
  if (now < t->next_tick)
    return;
  if (t->interval_ticks == 0) {
    /* One-shot fired. */
    t->expirations += 1;
    t->next_tick = 0;
    t->armed = 0;
    __atomic_sub_fetch(&g_armed_timerfds, 1, __ATOMIC_RELAXED);
  } else {
    u64 elapsed = now - t->next_tick;
    u64 ticks = 1 + elapsed / t->interval_ticks;
    t->expirations += ticks;
    t->next_tick += ticks * t->interval_ticks;
  }
}

static isize timerfd_read(struct vfs_handle *h, char *buf, usize len) {
  struct timerfd_state *t = (struct timerfd_state *)h->private_data;
  if (!t || len < sizeof(u64))
    return -EINVAL;
  while (1) {
    while (__atomic_test_and_set(&t->lock, __ATOMIC_ACQUIRE))
      scheduler_yield();
    timerfd_advance(t);
    if (t->expirations != 0)
      break;
    __atomic_clear(&t->lock, __ATOMIC_RELEASE);
    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    /* Sleep with a bounded deadline so we re-check even if the tick-hook wake
     * is missed; the ISR also wakes vfs_poll_chan, but this object blocks on
     * its own channel. */
    scheduler_block_on_timeout(t, 1);
  }
  u64 out = t->expirations;
  t->expirations = 0;
  __atomic_clear(&t->lock, __ATOMIC_RELEASE);
  memcpy(buf, &out, sizeof(u64));
  return (isize)sizeof(u64);
}

static int timerfd_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct timerfd_state *t = (struct timerfd_state *)h->private_data;
  pfd->revents = 0;
  if (!t)
    return 0;
  /* Lockless, read-only readiness check: poll() and epoll_wait() call this with
   * interrupts disabled (after scheduler_wait_prepare), so it must not take the
   * lock or yield. A timerfd is readable if expirations have already been
   * folded in, or if its deadline has elapsed (the actual count is computed on
   * the next read under the lock). */
  if (t->expirations != 0) {
    pfd->revents |= B1NIX_POLLIN;
  } else if (t->armed && t->next_tick != 0 &&
             scheduler_get_uptime_ticks() >= t->next_tick) {
    pfd->revents |= B1NIX_POLLIN;
  }
  return 0;
}

static void timerfd_release(struct vfs_handle *h) {
  struct timerfd_state *t = (struct timerfd_state *)h->private_data;
  if (!t)
    return;
  if (t->armed)
    __atomic_sub_fetch(&g_armed_timerfds, 1, __ATOMIC_RELAXED);
  kfree(t);
  h->private_data = 0;
}

static const struct vfs_file_ops timerfd_ops = {
    .read = timerfd_read,
    .poll = timerfd_poll,
    .release = timerfd_release,
};

int vfs_timerfd_create(int clockid, int flags) {
  if (clockid != B1NIX_CLOCK_REALTIME && clockid != B1NIX_CLOCK_MONOTONIC)
    return -EINVAL;
  if (flags & ~(B1NIX_TFD_CLOEXEC | B1NIX_TFD_NONBLOCK))
    return -EINVAL;
  struct timerfd_state *t = kzalloc(sizeof(*t));
  if (!t)
    return -ENOMEM;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_TIMERFD);
  if (!h) {
    kfree(t);
    return -ENFILE;
  }
  h->private_data = t;
  h->ops = &timerfd_ops;
  h->flags = (flags & B1NIX_TFD_NONBLOCK) ? B1NIX_O_NONBLOCK : 0;
  h->flags |= B1NIX_O_RDONLY;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  if (flags & B1NIX_TFD_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

static u64 timespec_to_ticks(const struct timespec *ts) {
  if (ts->tv_sec < 0 || ts->tv_nsec < 0)
    return 0;
  u64 ticks = (u64)ts->tv_sec * TICKS_PER_SEC;
  /* Round sub-tick remainders up to one tick so a 1 ms timer still fires. */
  u64 ns = (u64)ts->tv_nsec;
  if (ns > 0)
    ticks += (ns + (1000000000ULL / TICKS_PER_SEC) - 1) /
             (1000000000ULL / TICKS_PER_SEC);
  return ticks;
}

int vfs_timerfd_settime(int fd, int flags,
                        const struct b1nix_itimerspec *new_value,
                        struct b1nix_itimerspec *old_value) {
  (void)flags; /* TFD_TIMER_ABSTIME treated relative (single monotonic base) */
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_TIMERFD)
    return -EBADF;
  if (!new_value)
    return -EINVAL;
  struct timerfd_state *t = (struct timerfd_state *)h->private_data;
  if (!t)
    return -EINVAL;

  u64 value = timespec_to_ticks(&new_value->it_value);
  u64 interval = timespec_to_ticks(&new_value->it_interval);

  while (__atomic_test_and_set(&t->lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  if (old_value) {
    memset(old_value, 0, sizeof(*old_value));
    if (t->armed && t->next_tick != 0) {
      u64 now = scheduler_get_uptime_ticks();
      u64 rem = t->next_tick > now ? t->next_tick - now : 0;
      old_value->it_value.tv_sec = (i64)(rem / TICKS_PER_SEC);
      old_value->it_value.tv_nsec =
          (i64)((rem % TICKS_PER_SEC) * (1000000000ULL / TICKS_PER_SEC));
      old_value->it_interval.tv_sec = (i64)(t->interval_ticks / TICKS_PER_SEC);
      old_value->it_interval.tv_nsec = (i64)((t->interval_ticks % TICKS_PER_SEC) *
                                             (1000000000ULL / TICKS_PER_SEC));
    }
  }

  int was_armed = t->armed;
  if (value == 0) {
    /* Disarm. */
    t->armed = 0;
    t->next_tick = 0;
    t->interval_ticks = 0;
    if (was_armed)
      __atomic_sub_fetch(&g_armed_timerfds, 1, __ATOMIC_RELAXED);
  } else {
    t->next_tick = scheduler_get_uptime_ticks() + value;
    t->interval_ticks = interval;
    t->armed = 1;
    t->expirations = 0;
    if (!was_armed)
      __atomic_add_fetch(&g_armed_timerfds, 1, __ATOMIC_RELAXED);
  }
  __atomic_clear(&t->lock, __ATOMIC_RELEASE);
  return 0;
}

/* Called from the timer ISR (scheduler_on_timer_tick). Wakes blocked pollers
 * when timerfds are armed so they re-scan and notice fired timers. */
void eventpoll_timer_tick(void) {
  if (__atomic_load_n(&g_armed_timerfds, __ATOMIC_RELAXED) > 0)
    scheduler_wake_all(vfs_poll_chan);
}

/* ---- signalfd ---------------------------------------------------------- */

struct signalfd_state {
  u64 mask; /* bitmask of signals this fd reports (bit (sig-1)) */
};

static isize signalfd_read(struct vfs_handle *h, char *buf, usize len) {
  struct signalfd_state *s = (struct signalfd_state *)h->private_data;
  if (!s || len < sizeof(struct b1nix_signalfd_siginfo))
    return -EINVAL;

  usize max = len / sizeof(struct b1nix_signalfd_siginfo);
  usize produced = 0;
  while (produced == 0) {
    for (int sig = 1; sig < 32 && produced < max; sig++) {
      u64 bit = 1ULL << (sig - 1);
      if (!(s->mask & bit))
        continue;
      if (!scheduler_peek_pending_signals(bit))
        continue;
      if (!scheduler_consume_pending_signal(sig))
        continue;
      struct b1nix_signalfd_siginfo *si =
          (struct b1nix_signalfd_siginfo *)(buf +
                                            produced * sizeof(*si));
      memset(si, 0, sizeof(*si));
      si->ssi_signo = (u32)sig;
      if (current_task && current_task->user_image &&
          ((struct user_loaded_image *)current_task->user_image)->personality ==
              PERSONALITY_LINUX) {
        si->ssi_signo = (u32)b1nix_signo_to_linux(sig);
      }
      si->ssi_pid = (u32)scheduler_get_pid();
      produced++;
    }
    if (produced != 0)
      break;
    if (h->flags & B1NIX_O_NONBLOCK)
      return -EAGAIN;
    if (scheduler_signal_pending())
      return -ERESTARTSYS;
    /* Bounded sleep on the poll channel: a signal posted from another CPU
     * wakes vfs_poll_chan via scheduler_kill's wake path; the timeout bounds
     * any missed wake. */
    scheduler_block_on_timeout(vfs_poll_chan, 1);
  }
  return (isize)(produced * sizeof(struct b1nix_signalfd_siginfo));
}

static int signalfd_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct signalfd_state *s = (struct signalfd_state *)h->private_data;
  pfd->revents = 0;
  if (!s)
    return 0;
  if (scheduler_peek_pending_signals(s->mask) != 0)
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

static void signalfd_release(struct vfs_handle *h) {
  if (h->private_data) {
    kfree(h->private_data);
    h->private_data = 0;
  }
}

static const struct vfs_file_ops signalfd_ops = {
    .read = signalfd_read,
    .poll = signalfd_poll,
    .release = signalfd_release,
};

int vfs_signalfd(int fd, u64 mask, int flags) {
  if (flags & ~(B1NIX_SFD_CLOEXEC | B1NIX_SFD_NONBLOCK))
    return -EINVAL;

  /* fd == -1: create a new signalfd. Otherwise update the mask on an existing
   * one (POSIX signalfd4 semantics). */
  if (fd >= 0) {
    struct vfs_handle *h = scheduler_fd_get(fd);
    if (!h || h->kind != VFS_HANDLE_SIGNALFD)
      return -EINVAL;
    struct signalfd_state *s = (struct signalfd_state *)h->private_data;
    if (!s)
      return -EINVAL;
    s->mask = mask;
    return fd;
  }

  struct signalfd_state *s = kzalloc(sizeof(*s));
  if (!s)
    return -ENOMEM;
  s->mask = mask;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_SIGNALFD);
  if (!h) {
    kfree(s);
    return -ENFILE;
  }
  h->private_data = s;
  h->ops = &signalfd_ops;
  h->flags = (flags & B1NIX_SFD_NONBLOCK) ? B1NIX_O_NONBLOCK : 0;
  h->flags |= B1NIX_O_RDONLY;

  int nfd = scheduler_fd_alloc(h);
  if (nfd < 0) {
    vfs_handle_release(h);
    return nfd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  if (flags & B1NIX_SFD_CLOEXEC)
    scheduler_fd_flags_set(nfd, B1NIX_FD_CLOEXEC);
  return nfd;
}

/* ---- epoll ------------------------------------------------------------- */

#define EPOLL_MAX_WATCH 256

struct epoll_watch {
  int used;
  int fd;
  u32 events;       /* requested event mask (incl. EPOLLET / EPOLLONESHOT) */
  u64 data;         /* opaque user data echoed back in epoll_wait */
  u32 last_revents; /* edge-triggered state: events seen on the last scan */
};

struct epoll_state {
  volatile int lock;
  struct epoll_watch watch[EPOLL_MAX_WATCH];
};

static int epoll_close(struct vfs_handle *h) {
  /* nothing FD-specific to tear down; release frees the state */
  (void)h;
  return 0;
}

static void epoll_release(struct vfs_handle *h) {
  if (h->private_data) {
    kfree(h->private_data);
    h->private_data = 0;
  }
}

/* An epoll instance is ready when anything it watches is ready.
 *
 * This used to answer "never ready" unconditionally, which makes an epoll
 * descriptor useless anywhere except epoll_wait: nesting one inside poll(),
 * select() or another epoll — the ordinary way to aggregate two event loops
 * into one — left the outer wait sleeping forever while the inner set had work
 * to do. Every library that composes loops does this.
 *
 * Depth is bounded because an epoll may watch an epoll: a cycle would recurse
 * until the kernel stack ran out, and userspace is free to build one. Linux
 * caps nesting at 5; the same number here, after which an instance reports
 * itself not ready rather than following the loop. */
static u32 epoll_match(struct epoll_watch *w, u32 revents);

static int epoll_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  static __thread int depth; /* per-CPU in practice: the walk never sleeps */
  struct epoll_state *ep = h ? (struct epoll_state *)h->private_data : 0;

  pfd->revents = 0;
  if (!ep || depth >= 5)
    return 0;

  depth++;
  for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
    if (!ep->watch[i].used)
      continue;
    struct epoll_watch *w = &ep->watch[i];
    struct vfs_handle *th = scheduler_fd_get(w->fd);
    struct b1nix_pollfd inner;

    if (!th || !th->ops || !th->ops->poll)
      continue;
    inner.fd = w->fd;
    inner.events = (short)(w->events & 0xffff);
    inner.revents = 0;
    th->ops->poll(th, &inner);
    if (epoll_match(w, (u32)(unsigned short)inner.revents)) {
      pfd->revents = B1NIX_POLLIN;
      break;
    }
  }
  depth--;
  return 0;
}

static const struct vfs_file_ops epoll_ops = {
    .poll = epoll_poll,
    .close = epoll_close,
    .release = epoll_release,
};

int vfs_epoll_create(int flags) {
  if (flags & ~B1NIX_EPOLL_CLOEXEC)
    return -EINVAL;
  struct epoll_state *ep = kzalloc(sizeof(*ep));
  if (!ep)
    return -ENOMEM;

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_EPOLL);
  if (!h) {
    kfree(ep);
    return -ENFILE;
  }
  h->private_data = ep;
  h->ops = &epoll_ops;
  h->flags = B1NIX_O_RDWR;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  if (flags & B1NIX_EPOLL_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}

int vfs_epoll_ctl(int epfd, int op, int fd, struct b1nix_epoll_event *event) {
  struct vfs_handle *eh = scheduler_fd_get(epfd);
  if (!eh || eh->kind != VFS_HANDLE_EPOLL)
    return -EBADF;
  struct epoll_state *ep = (struct epoll_state *)eh->private_data;
  if (!ep)
    return -EBADF;
  if (fd == epfd)
    return -EINVAL;
  /* The target fd must be a valid open handle. */
  struct vfs_handle *th = scheduler_fd_get(fd);
  if (!th)
    return -EBADF;

  int rc = 0;
  while (__atomic_test_and_set(&ep->lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  /* Find an existing registration for fd. */
  int existing = -1, free_slot = -1;
  for (int i = 0; i < EPOLL_MAX_WATCH; i++) {
    if (ep->watch[i].used) {
      if (ep->watch[i].fd == fd) {
        existing = i;
        break;
      }
    } else if (free_slot < 0) {
      free_slot = i;
    }
  }

  switch (op) {
  case B1NIX_EPOLL_CTL_ADD:
    if (existing >= 0) {
      rc = -EEXIST;
      break;
    }
    if (free_slot < 0) {
      rc = -ENOSPC;
      break;
    }
    if (!event) {
      rc = -EFAULT;
      break;
    }
    ep->watch[free_slot].used = 1;
    ep->watch[free_slot].fd = fd;
    ep->watch[free_slot].events = event->events;
    ep->watch[free_slot].data = event->data.u64;
    ep->watch[free_slot].last_revents = 0;
    break;
  case B1NIX_EPOLL_CTL_MOD:
    if (existing < 0) {
      rc = -ENOENT;
      break;
    }
    if (!event) {
      rc = -EFAULT;
      break;
    }
    ep->watch[existing].events = event->events;
    ep->watch[existing].data = event->data.u64;
    ep->watch[existing].last_revents = 0;
    break;
  case B1NIX_EPOLL_CTL_DEL:
    if (existing < 0) {
      rc = -ENOENT;
      break;
    }
    ep->watch[existing].used = 0;
    break;
  default:
    rc = -EINVAL;
    break;
  }

  __atomic_clear(&ep->lock, __ATOMIC_RELEASE);
  return rc;
}

/* Translate poll revents into epoll events for a single watch, honoring the
 * watch's requested mask. POLLERR/POLLHUP are always reported (Linux does the
 * same — they cannot be masked out). */
static u32 epoll_match(struct epoll_watch *w, u32 revents) {
  u32 want = w->events & (B1NIX_EPOLLIN | B1NIX_EPOLLOUT | B1NIX_EPOLLPRI |
                          B1NIX_EPOLLRDHUP);
  u32 out = revents & (want | B1NIX_EPOLLERR | B1NIX_EPOLLHUP);
  return out;
}

int vfs_epoll_wait(int epfd, struct b1nix_epoll_event *events, int maxevents,
                   int timeout) {
  if (maxevents <= 0)
    return -EINVAL;
  struct vfs_handle *eh = scheduler_fd_get(epfd);
  if (!eh || eh->kind != VFS_HANDLE_EPOLL)
    return -EBADF;
  struct epoll_state *ep = (struct epoll_state *)eh->private_data;
  if (!ep)
    return -EBADF;

  u64 start_ticks = scheduler_get_uptime_ticks();
  /* timeout in ms; <0 means wait forever, matching epoll_wait(2). */
  u64 timeout_ticks =
      (timeout < 0) ? (u64)-1 : ((u64)timeout) / (1000ULL / TICKS_PER_SEC);

  if (timeout > 0) {
    u64 ticks = timeout_ticks > 0 ? timeout_ticks : 1;
    current_task->wake_tick = start_ticks + ticks;
  }

  while (1) {
    /* Publish BLOCKED on vfs_poll_chan BEFORE scanning, mirroring sys_poll's
     * SMP lost-wakeup fix: a watched fd becoming ready (waker on another CPU)
     * either is seen by this scan or observes our BLOCKED state. wait_prepare
     * disables interrupts, so the scan below must NOT sleep/yield — hence the
     * watch array is read locklessly (a fixed-size POD array; a concurrent
     * epoll_ctl on the same epfd is a POSIX-undefined race, and a torn read at
     * worst polls a stale fd, which scheduler_fd_get rejects). */
    scheduler_wait_prepare(vfs_poll_chan);

    int nready = 0;
    for (int i = 0; i < EPOLL_MAX_WATCH && nready < maxevents; i++) {
      if (!ep->watch[i].used)
        continue;
      struct epoll_watch *w = &ep->watch[i];
      struct vfs_handle *th = scheduler_fd_get(w->fd);
      struct b1nix_pollfd pfd;
      pfd.fd = w->fd;
      pfd.events = (short)(w->events & 0xffff);
      pfd.revents = 0;
      if (!th) {
        pfd.revents = B1NIX_POLLNVAL;
      } else if (th->ops && th->ops->poll) {
        th->ops->poll(th, &pfd);
        pfd.revents &= (pfd.events | B1NIX_POLLERR | B1NIX_POLLHUP |
                        B1NIX_POLLNVAL);
      }

      u32 matched = epoll_match(w, (u32)(unsigned short)pfd.revents);
      if (matched == 0) {
        w->last_revents = 0;
        continue;
      }

      /* Edge-triggered: only report when the readiness set changed from the
       * last scan (rising edge). Level-triggered reports on every scan. */
      if (w->events & B1NIX_EPOLLET) {
        if (matched == w->last_revents) {
          continue;
        }
      }
      w->last_revents = matched;

      events[nready].events = matched;
      events[nready].data.u64 = w->data;
      nready++;

      /* EPOLLONESHOT: disable the watch after one report (mask down to 0). */
      if (w->events & B1NIX_EPOLLONESHOT)
        w->events &= ~(B1NIX_EPOLLIN | B1NIX_EPOLLOUT | B1NIX_EPOLLPRI |
                       B1NIX_EPOLLRDHUP);
    }

    int timed_out = 0;
    if (nready == 0 && timeout > 0) {
      u64 now = scheduler_get_uptime_ticks();
      if (now - start_ticks >= timeout_ticks)
        timed_out = 1;
    }

    if (nready > 0 || timeout == 0 || timed_out) {
      scheduler_wait_cancel();
      current_task->wake_tick = 0;
      return nready;
    }

    if (scheduler_signal_pending()) {
      scheduler_wait_cancel();
      current_task->wake_tick = 0;
      return -ERESTARTSYS;
    }

    scheduler_wait_commit();
  }
}

/* ---- memfd file sealing ------------------------------------------------ */

int vfs_fcntl_add_seals(int fd, u32 seals) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_NODE || !h->node || !h->node->inode)
    return -EINVAL;
  struct vfs_inode *in = h->node->inode;
  if (!in->seals_allowed)
    return -EINVAL; /* not a sealable memfd */
  if (seals & ~(u32)(B1NIX_F_SEAL_SEAL | B1NIX_F_SEAL_SHRINK |
                     B1NIX_F_SEAL_GROW | B1NIX_F_SEAL_WRITE))
    return -EINVAL;
  if (in->seals & B1NIX_F_SEAL_SEAL)
    return -EPERM; /* further sealing forbidden */
  /* F_SEAL_WRITE requires no outstanding writable mappings in Linux; b1nix
   * does not track shared memfd mappings per-inode, so we accept it. */
  in->seals |= seals;
  return 0;
}

int vfs_fcntl_get_seals(int fd) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_NODE || !h->node || !h->node->inode)
    return -EINVAL;
  if (!h->node->inode->seals_allowed)
    return -EINVAL;
  return (int)h->node->inode->seals;
}
