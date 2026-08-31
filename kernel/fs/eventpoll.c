#include <b1nix/lapic.h>
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
 * The kernel timer tick drives timerfd: scheduler_on_timer_tick calls
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
#include <b1nix/lapic.h>
#include <b1nix/user.h>
#include <stdlib.h>
#include <string.h>

/* The tick rate the timer was actually programmed with -- read, never written
 * out. It has not been 100 Hz since the LAPIC timer could be calibrated, and a
 * comment claiming otherwise is how a reader talks themselves into hardcoding
 * it again. */
#define TICKS_PER_SEC SCHED_TICKS_PER_SEC /* see sched.h */

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

/* ---- pidfd ------------------------------------------------------------- */
/*
 * pidfd_open(2). A descriptor that names a PROCESS rather than a number that
 * happens to identify one today.
 *
 * The distinction is the whole reason it exists: a pid is reused, so a manager
 * that stores one and later signals it can, after an unlucky sequence of
 * exits, signal a process it has never heard of. A descriptor cannot be
 * reused while it is held. systemd from 254 onwards is built around that
 * guarantee -- its PidRef pairs a pid with a pidfd and re-verifies one against
 * the other -- and it takes a pidfd on ITSELF before it will run a boot.
 * Without the call, PID 1 reported "Failed to acquire PID reference on
 * ourselves" and then sat in its event loop with no jobs, for ever.
 *
 * What a pidfd has to do here:
 *   poll   readable once the process has exited, which is how an event loop
 *          learns of a death without SIGCHLD;
 *   fstat  a stable, unique st_ino, which is the "pidfd id" a caller stores to
 *          tell two references apart;
 *   signal pidfd_send_signal(2) delivers to the process the descriptor holds;
 *   read   EINVAL, as on Linux for a non-PIDFD_THREAD descriptor.
 */

/* Ids are handed out in sequence and never reused within a boot, which is
 * exactly the promise the id is for. */
static u64 g_pidfd_next_id = 1;
static volatile int g_pidfd_id_lock = 0;

struct pidfd_state {
  usize pid;
  u64 id;
};

static u64 pidfd_alloc_id(void) {
  while (__atomic_test_and_set(&g_pidfd_id_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
  u64 id = g_pidfd_next_id++;
  __atomic_clear(&g_pidfd_id_lock, __ATOMIC_RELEASE);
  return id;
}

/* Has the process this descriptor names finished?
 *
 * "Gone from the table" and "sitting in the table as a zombie" are both an
 * exit as far as a poller is concerned: the caller is waiting to be told it
 * may reap, and a zombie is precisely a process waiting to be reaped. */
static int pidfd_process_exited(const struct pidfd_state *p) {
  if (!p)
    return 1;
  struct task *t = scheduler_task_by_pid(p->pid);
  if (!t)
    return 1;
  enum task_state st = t->state;
  return st == TASK_DEAD || st == TASK_REAPING || st == TASK_UNUSED;
}

static isize pidfd_read(struct vfs_handle *h, char *buf, usize len) {
  (void)h;
  (void)buf;
  (void)len;
  /* Linux: reading a process-directed pidfd is EINVAL. Only PIDFD_THREAD
   * descriptors read back an exit status, and those are refused at open. */
  return -EINVAL;
}

static int pidfd_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct pidfd_state *p = (struct pidfd_state *)h->private_data;
  pfd->revents = 0;
  if (!p)
    return 0;
  if (pidfd_process_exited(p))
    pfd->revents |= B1NIX_POLLIN;
  return 0;
}

static void pidfd_release(struct vfs_handle *h) {
  if (h->private_data) {
    kfree(h->private_data);
    h->private_data = 0;
  }
}

static const struct vfs_file_ops pidfd_ops = {
    .read = pidfd_read,
    .poll = pidfd_poll,
    .release = pidfd_release,
};

usize vfs_pidfd_pid(struct vfs_handle *h) {
  if (!h || h->kind != VFS_HANDLE_PIDFD || !h->private_data)
    return 0;
  return ((struct pidfd_state *)h->private_data)->pid;
}

u64 vfs_pidfd_id(struct vfs_handle *h) {
  if (!h || h->kind != VFS_HANDLE_PIDFD || !h->private_data)
    return 0;
  return ((struct pidfd_state *)h->private_data)->id;
}

int vfs_pidfd_open(usize pid, int flags) {
  /* PIDFD_NONBLOCK is O_NONBLOCK's value; PIDFD_THREAD (O_EXCL) is refused
   * rather than silently treated as a process pidfd, because the two differ in
   * what waiting on the descriptor means and a caller that asked for one and
   * got the other would be told a lie. */
  if (flags & ~(int)B1NIX_O_NONBLOCK)
    return -EINVAL;
  if (pid == 0)
    return -EINVAL;

  struct task *t = scheduler_task_by_pid(pid);
  if (!t)
    return -ESRCH;
  if (t->state == TASK_DEAD || t->state == TASK_REAPING ||
      t->state == TASK_UNUSED)
    return -ESRCH;
  /* Linux: a pid that names a thread rather than a thread group leader is
   * EINVAL without PIDFD_THREAD. */
  if (task_tgid(t) != pid)
    return -EINVAL;

  struct pidfd_state *p = kzalloc(sizeof(*p));
  if (!p)
    return -ENOMEM;
  p->pid = pid;
  /* The identity of the PROCESS, not of this descriptor: two pidfds for one
   * process must report the same inode, which is what a caller compares to
   * decide the descriptor still names what it named before. The per-descriptor
   * counter is the fallback for a task that has already gone. */
  p->id = scheduler_task_pidfs_ino(pid);
  if (!p->id)
    p->id = pidfd_alloc_id();

  struct vfs_handle *h = alloc_raw_handle(VFS_HANDLE_PIDFD);
  if (!h) {
    kfree(p);
    return -ENFILE;
  }
  h->private_data = p;
  h->ops = &pidfd_ops;
  h->flags = (u32)(flags & B1NIX_O_NONBLOCK);
  h->flags |= B1NIX_O_RDONLY;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h);
    return fd == -ENOMEM ? -ENOMEM : -EMFILE;
  }
  /* Linux always sets close-on-exec on a pidfd; there is no flag to ask for
   * it because there is no case for inheriting one across exec. */
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
  int clockid;       /* which clock an ABSTIME deadline is measured against */
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
  /* The clocks Linux lets a timerfd use. b1nix has one monotonic base, so
   * MONOTONIC and BOOTTIME are the same clock here, and the _ALARM variants
   * differ from their bases only in waking a suspended machine — which this
   * kernel never does. Rejecting them made sd-event fall back and, for
   * BOOTTIME, gave up on the timer entirely. */
  if (clockid != B1NIX_CLOCK_REALTIME && clockid != B1NIX_CLOCK_MONOTONIC &&
      clockid != B1NIX_CLOCK_BOOTTIME && clockid != B1NIX_CLOCK_REALTIME_ALARM &&
      clockid != B1NIX_CLOCK_BOOTTIME_ALARM)
    return -EINVAL;
  if (flags & ~(B1NIX_TFD_CLOEXEC | B1NIX_TFD_NONBLOCK))
    return -EINVAL;
  struct timerfd_state *t = kzalloc(sizeof(*t));
  if (!t)
    return -ENOMEM;
  t->clockid = clockid;

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

/* Ticks a timespec is worth, saturating rather than wrapping.
 *
 * systemd arms its "the wall clock was stepped" watch for TIME_T_MAX seconds.
 * Multiplying that by the tick rate wraps a u64 back to a small number, so the
 * timer that must never fire fired at once: epoll reported the descriptor
 * readable on every pass, systemd rebuilt the watch, and PID 1 spun in its
 * event loop printing "Looping too fast" instead of starting any unit. */
#define TIMERFD_TICKS_MAX (~(u64)0 / 4)

static u64 timespec_to_ticks(const struct timespec *ts) {
  if (ts->tv_sec < 0 || ts->tv_nsec < 0)
    return 0;
  if ((u64)ts->tv_sec > TIMERFD_TICKS_MAX / TICKS_PER_SEC)
    return TIMERFD_TICKS_MAX;
  u64 ticks = (u64)ts->tv_sec * TICKS_PER_SEC;
  /* Round sub-tick remainders up to one tick so a 1 ms timer still fires. */
  u64 ns = (u64)ts->tv_nsec;
  if (ns > 0)
    ticks += (ns + (1000000000ULL / TICKS_PER_SEC) - 1) /
             (1000000000ULL / TICKS_PER_SEC);
  return ticks > TIMERFD_TICKS_MAX ? TIMERFD_TICKS_MAX : ticks;
}

/* "Now" on the clock a timerfd was created with, in ticks, so an ABSTIME
 * deadline can be turned into a delay. The monotonic value is the same uptime
 * tick count next_tick is measured in; the realtime value is the wall clock,
 * which is what a caller comparing against clock_gettime(CLOCK_REALTIME)
 * means. */
static u64 timerfd_now_ticks(int clockid) {
  if (clockid == B1NIX_CLOCK_REALTIME || clockid == B1NIX_CLOCK_REALTIME_ALARM)
    return (u64)vfs_get_unix_time() * TICKS_PER_SEC;
  return scheduler_get_uptime_ticks();
}

int vfs_timerfd_settime(int fd, int flags,
                        const struct b1nix_itimerspec *new_value,
                        struct b1nix_itimerspec *old_value) {
  struct vfs_handle *h = scheduler_fd_get(fd);
  if (!h || h->kind != VFS_HANDLE_TIMERFD)
    return -EBADF;
  if (!new_value)
    return -EINVAL;
  struct timerfd_state *t = (struct timerfd_state *)h->private_data;
  if (!t)
    return -EINVAL;

  /* it_value == 0 disarms, whatever the flags say. */
  int disarm = (new_value->it_value.tv_sec == 0 &&
                new_value->it_value.tv_nsec == 0);
  u64 value = timespec_to_ticks(&new_value->it_value);
  u64 interval = timespec_to_ticks(&new_value->it_interval);

  /* TFD_TIMER_ABSTIME: it_value is a DEADLINE on this timerfd's clock, not a
   * delay. Treating it as a delay put every one of systemd's timeouts (which
   * are all absolute) roughly fifty years into the future, so no timeout it
   * set ever fired. TFD_TIMER_CANCEL_ON_SET is accepted and has no effect:
   * this kernel does not report a stepped wall clock to a sleeping timer. */
  if (!disarm && (flags & B1NIX_TFD_TIMER_ABSTIME)) {
    /* Resolve the deadline on the clock the caller read it from.
     *
     * This converted the deadline to scheduler ticks and compared it against
     * the uptime tick count — a different, coarser clock than the
     * CLOCK_MONOTONIC userspace used to compute it. The two need only disagree
     * by more than the interval being timed for every deadline to look already
     * past, and a compositor asking for the next frame in sixteen milliseconds
     * then gets a timer that fires immediately, for ever: it spins repainting
     * and never services anything else, which is what "the compositor hangs
     * the machine" turned out to be.
     *
     * The monotonic case is computed in nanoseconds against the same counter
     * clock_gettime answers from, and only the resulting delay is turned into
     * ticks. The realtime case keeps the seconds-based comparison, which is
     * the clock those deadlines are actually expressed on. */
    if (t->clockid == B1NIX_CLOCK_REALTIME ||
        t->clockid == B1NIX_CLOCK_REALTIME_ALARM) {
      u64 now_clock = timerfd_now_ticks(t->clockid);

      value = value > now_clock ? value - now_clock : 1;
    } else {
      extern u64 arch_tsc_monotonic_ns(void);
      u64 now_ns = arch_tsc_monotonic_ns();
      u64 want_ns = (u64)new_value->it_value.tv_sec * 1000000000ull +
                    (u64)new_value->it_value.tv_nsec;
      u64 tick_ns = 1000000000ull / TICKS_PER_SEC;
      u64 delay_ns = want_ns > now_ns ? want_ns - now_ns : 0;

      value = (delay_ns + tick_ns - 1) / tick_ns;
      if (value == 0)
        value = 1; /* already due: fire on the next tick, not never */
    }
  }

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
  if (disarm || value == 0) {
    /* Disarm. */
    t->armed = 0;
    t->next_tick = 0;
    t->interval_ticks = 0;
    if (was_armed)
      __atomic_sub_fetch(&g_armed_timerfds, 1, __ATOMIC_RELAXED);
  } else {
    u64 base = scheduler_get_uptime_ticks();
    t->next_tick = (value > TIMERFD_TICKS_MAX - base) ? TIMERFD_TICKS_MAX
                                                      : base + value;
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

/* Starting size only. The table grows on demand — see epoll_grow. A fixed
 * ceiling here is a wall a compositor walks into: sway watches a descriptor
 * per client, per input device and per timer, and a browser many more. */
#define EPOLL_INIT_WATCH 64

struct epoll_watch {
  int used;
  int fd;
  /* The open file this watch was registered against, with a reference held.
   *
   * Watching by descriptor number alone is watching nothing: the number is
   * reused the moment the file is closed, and the watch then reports the new
   * file's readiness together with the OLD file's user data. Userspace
   * dereferences that pointer, which by then belongs to whatever allocation
   * took the freed memory. It is how sway came to call a function pointer made
   * of pixel data. Linux keeps a file reference for exactly this reason; so do
   * we, and the scan polls this file rather than looking the number up again. */
  struct vfs_handle *file;
  u32 events;       /* requested event mask (incl. EPOLLET / EPOLLONESHOT) */
  u64 data;         /* opaque user data echoed back in epoll_wait */
  u32 last_revents; /* edge-triggered state: events seen on the last scan */
};

/* Retired tables, kept until the epoll fd is closed.
 *
 * Growth cannot free the table it replaces: epoll_wait scans it locklessly
 * with interrupts disabled, and freeing underneath that scan is a use-after
 * -free in the kernel. There are at most log2(watches) of these. */
struct epoll_old_table {
  struct epoll_old_table *next;
  struct epoll_watch *watch;
};

struct epoll_state {
  volatile int lock;
  struct epoll_watch *watch;
  int capacity;
  struct epoll_old_table *retired;
};

static int epoll_close(struct vfs_handle *h) {
  /* nothing FD-specific to tear down; release frees the state */
  (void)h;
  return 0;
}

static void epoll_release(struct vfs_handle *h) {
  if (h->private_data) {
    struct epoll_state *ep = (struct epoll_state *)h->private_data;

    for (int i = 0; i < ep->capacity; i++)
      if (ep->watch[i].used && ep->watch[i].file)
        vfs_handle_release(ep->watch[i].file);
    while (ep->retired) {
      struct epoll_old_table *t = ep->retired;

      ep->retired = t->next;
      kfree(t->watch);
      kfree(t);
    }
    kfree(ep->watch);
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
  /* Per-CPU, spelled out.
   *
   * This was a `static __thread` variable, which in kernel code is not what it
   * looks like: this kernel keeps %fs pointing at the CALLING THREAD'S user
   * TLS base so that userspace %fs:N works, so a __thread variable here
   * resolves through the user's TLS and the increment below wrote four bytes
   * into the calling process's own thread-control block. Every program with an
   * event loop was quietly corrupting its own libc state — which is why a
   * compositor died following pointers out of its heap while nothing could be
   * caught writing to them. */
  static int depth[MAX_CPUS];
  struct epoll_state *ep = h ? (struct epoll_state *)h->private_data : 0;
  int dcpu = (int)percpu_read(cpu_id);

  if (dcpu < 0 || dcpu >= MAX_CPUS)
    dcpu = 0;

  pfd->revents = 0;
  if (!ep || depth[dcpu] >= 5)
    return 0;

  depth[dcpu]++;
  for (int i = 0; i < ep->capacity; i++) {
    if (!ep->watch[i].used)
      continue;
    struct epoll_watch *w = &ep->watch[i];
    struct vfs_handle *th = w->file;
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
  depth[dcpu]--;
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

  if (ep) {
    ep->watch = kzalloc(sizeof(struct epoll_watch) * EPOLL_INIT_WATCH);
    if (!ep->watch) {
      kfree(ep);
      ep = 0;
    } else {
      ep->capacity = EPOLL_INIT_WATCH;
    }
  }
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

/* Double the watch table and return the index of a fresh slot.
 *
 * The old table is retired rather than freed: epoll_wait scans it without the
 * lock and with interrupts disabled, so it may still be reading it. Called
 * with ep->lock held. Returns -1 if there is no memory. */
static int epoll_grow(struct epoll_state *ep) {
  int new_cap = ep->capacity ? ep->capacity * 2 : EPOLL_INIT_WATCH;
  struct epoll_watch *nw;
  struct epoll_old_table *old;

  if (new_cap <= ep->capacity)
    return -1; /* overflow */
  nw = kzalloc(sizeof(struct epoll_watch) * (usize)new_cap);
  if (!nw)
    return -1;
  old = kzalloc(sizeof(*old));
  if (!old) {
    kfree(nw);
    return -1;
  }
  memcpy(nw, ep->watch, sizeof(struct epoll_watch) * (usize)ep->capacity);
  old->watch = ep->watch;
  old->next = ep->retired;
  ep->retired = old;
  /* Publish the table before the size, so a scan that sees the new capacity
   * is reading the table that has the slots. */
  __atomic_store_n(&ep->watch, nw, __ATOMIC_RELEASE);
  int slot = ep->capacity;
  __atomic_store_n(&ep->capacity, new_cap, __ATOMIC_RELEASE);
  return slot;
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

  /* Find an existing registration for fd.
   *
   * A registration is keyed by the descriptor AND the file behind it, as it is
   * in Linux — not by the descriptor number alone. The number is reused the
   * moment the program closes something, and a watch left over from the
   * PREVIOUS occupant made the next EPOLL_CTL_ADD on that number answer
   * EEXIST: systemd-udevd creates and closes descriptors while it builds its
   * event loop, and died on "Failed to create SIGTERM event source: File
   * exists" every single boot.
   *
   * A stale watch — same number, different file — is dropped here rather than
   * reported. Closing the descriptor is what ended that registration; this is
   * simply where the kernel notices, since nothing else holds a back-pointer
   * from a handle to the epoll sets watching it. */
  int existing = -1, free_slot = -1;
  for (int i = 0; i < ep->capacity; i++) {
    if (ep->watch[i].used) {
      if (ep->watch[i].fd == fd) {
        if (ep->watch[i].file == th) {
          existing = i;
          break;
        }
        /* Stale: this number names something else now. */
        if (ep->watch[i].file)
          vfs_handle_release(ep->watch[i].file);
        ep->watch[i].used = 0;
        ep->watch[i].file = 0;
        if (free_slot < 0)
          free_slot = i;
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
    if (!event) {
      rc = -EFAULT;
      break;
    }
    if (free_slot < 0) {
      free_slot = epoll_grow(ep);
      if (free_slot < 0) {
        rc = -ENOMEM;
        break;
      }
    }
    /* Hold the file for as long as the watch names it. */
    th = scheduler_fd_get_retain(fd);
    if (!th) {
      rc = -EBADF;
      break;
    }
    ep->watch[free_slot].file = th;
    ep->watch[free_slot].fd = fd;
    ep->watch[free_slot].events = event->events;
    ep->watch[free_slot].data = event->data.u64;
    ep->watch[free_slot].last_revents = 0;
    __atomic_store_n(&ep->watch[free_slot].used, 1, __ATOMIC_RELEASE);
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
    __atomic_store_n(&ep->watch[existing].used, 0, __ATOMIC_RELEASE);
    if (ep->watch[existing].file) {
      vfs_handle_release(ep->watch[existing].file);
      ep->watch[existing].file = 0;
    }
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
      (timeout < 0) ? (u64)-1 : SCHED_MS_TO_TICKS((u64)timeout);

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
    for (int i = 0; i < ep->capacity && nready < maxevents; i++) {
      if (!__atomic_load_n(&ep->watch[i].used, __ATOMIC_ACQUIRE))
        continue;
      struct epoll_watch *w = &ep->watch[i];
      struct vfs_handle *th = w->file;

      /* Linux drops a registration once every DESCRIPTOR naming the file is
       * closed — epoll's own hold on the file does not keep the watch alive
       * (epoll(7): "removed ... after all file descriptors referring to it are
       * closed"). Userspace relies on it: wl_event_source_remove() closes the
       * descriptor and frees the source WITHOUT an EPOLL_CTL_DEL. Keeping the
       * watch past that reports readiness with a pointer to freed memory, and
       * the program calls through whatever has since moved in — for sway, a
       * function pointer made of background pixels.
       *
       * Our hold is the only remaining reference exactly when the last
       * descriptor is gone, so that is the test. */
      /* Ask the descriptor table, not the reference count.
       *
       * Counting references cannot answer this: the count also rises for a
       * message queued with SCM_RIGHTS, for an inherited copy, and for any
       * transient hold, so a watch on a closed descriptor kept a count of two
       * or three and never looked closed at all. What the rule actually says
       * is simpler — the registration lasts as long as a descriptor still
       * names this open file — so look the number up in the owner's table and
       * compare the file it lands on. A closed descriptor finds nothing; a
       * reused number finds somebody else. */
      if (th && scheduler_fd_get(w->fd) != th) {
        __atomic_store_n(&w->used, 0, __ATOMIC_RELEASE);
        w->file = 0;
        vfs_handle_release(th);
        continue;
      }
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
      /* Last few pointers this kernel handed back, for the fault reporter.
       * If a program dies dereferencing one of these, the readiness report
       * named an object it had already given up; if the address is absent,
       * it came from somewhere else and this path is not the culprit. */
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
