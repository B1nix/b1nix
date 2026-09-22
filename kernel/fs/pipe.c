/* SPDX-License-Identifier: GPL-2.0-only */
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/resource_caps.h>
#include <b1nix/sched.h>
#include <b1nix/arch.h>
#include <b1nix/spinlock.h>
#include <stdlib.h>
#include <string.h>

struct vfs_pipe pipes[MAX_VFS_PIPES_CEIL];
/* Guards the pipes[] free-slot search in vfs_pipe(). Without it, two CPUs
 * racing through vfs_pipe at the same instant could each pick the same
 * `!used` slot — both then memset()+used=1 on the SAME struct, sharing one
 * underlying buffer; when one side closed, readers would drop to 0 and the
 * other side would get an unexpected SIGPIPE/EPIPE. This was a real hang
 * under make -j8 (8 concurrent fork chains all calling vfs_pipe). The
 * per-pipe lock inside struct vfs_pipe only protects in-flight read/write
 * once the slot is owned; the search-and-claim transition is what needs
 * mutual exclusion. */
static spinlock_t pipe_pool_lock = SPINLOCK_INIT;

/* How many pipe data buffers are allocated, and how many are kept when their
 * pipes close. See pipe_release. */
static volatile int g_pipe_buffers_held;
#define PIPE_BUFFER_CACHE 64

static isize pipe_read(struct vfs_handle *h, char *buf, usize size) {
  struct vfs_pipe *pipe = (struct vfs_pipe *)h->private_data;
  if (!pipe || !pipe->used) return -EIO;
  
  while (1) {
    while (__atomic_test_and_set(&pipe->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
    if (pipe->size == 0) {
      if (pipe->writers == 0) { __atomic_clear(&pipe->lock, __ATOMIC_RELEASE); return 0; }
      if (h->flags & B1NIX_O_NONBLOCK) { __atomic_clear(&pipe->lock, __ATOMIC_RELEASE); return -EAGAIN; }
      if (scheduler_signal_pending()) { __atomic_clear(&pipe->lock, __ATOMIC_RELEASE); return -ERESTARTSYS; }
      interrupts_disable();
      current_task->wait_chan = pipe;
      scheduler_lease_clear_here(__func__);
      current_task->state = TASK_BLOCKED;
      __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
      scheduler_yield();
      interrupts_enable();
      continue;
    }
    break;
  }
  
  usize to_r = size < pipe->size ? size : pipe->size;
  for (usize i = 0; i < to_r; i++) {
    buf[i] = pipe->buffer[pipe->read_pos];
    pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUFFER_SIZE;
  }
  pipe->size -= to_r;
  __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
  
  /* Wake up writers */
  scheduler_wake_all(pipe);
  scheduler_wake_all(vfs_poll_chan);
  
  return (isize)to_r;
}

static isize pipe_write(struct vfs_handle *h, const char *buf, usize size) {
  struct vfs_pipe *pipe = (struct vfs_pipe *)h->private_data;
  if (!pipe || !pipe->used) return -EIO;

  while (1) {
    while (__atomic_test_and_set(&pipe->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
    if (pipe->readers == 0) {
      __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
      scheduler_kill(scheduler_get_pid(), SIGPIPE);
      return -EPIPE;
    }
    usize free_space = PIPE_BUFFER_SIZE - pipe->size;
    if (free_space == 0) {
      if (h->flags & B1NIX_O_NONBLOCK) { __atomic_clear(&pipe->lock, __ATOMIC_RELEASE); return -EAGAIN; }
      if (scheduler_signal_pending()) { __atomic_clear(&pipe->lock, __ATOMIC_RELEASE); return -ERESTARTSYS; }
      interrupts_disable();
      current_task->wait_chan = pipe;
      scheduler_lease_clear_here(__func__);
      current_task->state = TASK_BLOCKED;
      __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
      scheduler_yield();
      interrupts_enable();
      continue;
    }
    break;
  }
  
  usize free_space = PIPE_BUFFER_SIZE - pipe->size;
  usize to_w = size < free_space ? size : free_space;
  for (usize i = 0; i < to_w; i++) {
    pipe->buffer[pipe->write_pos] = buf[i];
    pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUFFER_SIZE;
  }
  pipe->size += to_w;
  __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
  
  /* Wake up readers */
  scheduler_wake_all(pipe);
  scheduler_wake_all(vfs_poll_chan);
  
  return (isize)to_w;
}

/* tee(2): duplicate up to `len` bytes from one pipe to another WITHOUT
 * consuming the source. Both descriptors must name pipes, and they must be
 * different pipes (Linux returns EINVAL for a self-tee). Both pipe locks are
 * taken in address order so two concurrent tees in opposite directions cannot
 * deadlock. Copies at most what the source holds and the destination can take,
 * which is exactly tee's contract (a short return is normal). */
isize vfs_pipe_tee(struct vfs_handle *in, struct vfs_handle *out, usize len) {
  if (!in || !out || in->kind != VFS_HANDLE_PIPE_READ ||
      out->kind != VFS_HANDLE_PIPE_WRITE)
    return -EINVAL;
  struct vfs_pipe *src = (struct vfs_pipe *)in->private_data;
  struct vfs_pipe *dst = (struct vfs_pipe *)out->private_data;
  if (!src || !dst || !src->used || !dst->used)
    return -EIO;
  if (src == dst)
    return -EINVAL;

  struct vfs_pipe *first = src < dst ? src : dst;
  struct vfs_pipe *second = src < dst ? dst : src;
  while (__atomic_test_and_set(&first->lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
  while (__atomic_test_and_set(&second->lock, __ATOMIC_ACQUIRE))
    scheduler_yield();

  isize res;
  if (dst->readers == 0) {
    res = -EPIPE;
    goto out_unlock;
  }
  usize avail = src->size;
  usize space = PIPE_BUFFER_SIZE - dst->size;
  usize n = len;
  if (n > avail)
    n = avail;
  if (n > space)
    n = space;
  if (n == 0) {
    /* Nothing to copy: a writer-less empty source is EOF, otherwise the
     * caller should retry (or block, which tee never does here). */
    res = (avail == 0 && src->writers == 0) ? 0
          : (avail == 0)                    ? -EAGAIN
                                            : -EAGAIN;
    goto out_unlock;
  }
  usize rp = src->read_pos;
  for (usize i = 0; i < n; i++) {
    dst->buffer[dst->write_pos] = src->buffer[rp];
    dst->write_pos = (dst->write_pos + 1) % PIPE_BUFFER_SIZE;
    rp = (rp + 1) % PIPE_BUFFER_SIZE;
  }
  dst->size += n; /* src->size deliberately untouched — tee does not consume */
  res = (isize)n;

out_unlock:
  __atomic_clear(&second->lock, __ATOMIC_RELEASE);
  __atomic_clear(&first->lock, __ATOMIC_RELEASE);
  if (res > 0) {
    scheduler_wake_all(dst);
    scheduler_wake_all(vfs_poll_chan);
  }
  return res;
}

static int pipe_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct vfs_pipe *pipe = (struct vfs_pipe *)h->private_data;
  if (!pipe || !pipe->used) { pfd->revents = B1NIX_POLLHUP; return 0; }
  pfd->revents = 0;
  if (h->kind == VFS_HANDLE_PIPE_READ) {
    if (pipe->size > 0) pfd->revents |= B1NIX_POLLIN;
    /* A FIFO's reader hangs up only once a writer has come and gone since it
     * opened (Linux compares the writer count at open, pipe->w_counter). A
     * reader opened O_NONBLOCK before any writer waits in poll for the first
     * one: that is how crun holds a container back until `crun start`, and
     * reporting POLLHUP straight away let the container run first. */
    if (pipe->writers == 0 &&
        (!h->node || pipe->writer_opens != (u32)h->offset))
      pfd->revents |= B1NIX_POLLHUP;
  } else if (h->kind == VFS_HANDLE_PIPE_WRITE) {
    if (pipe->size < PIPE_BUFFER_SIZE) pfd->revents |= B1NIX_POLLOUT;
    if (pipe->readers == 0) pfd->revents |= B1NIX_POLLERR;
  }
  return 0;
}

static void pipe_release(struct vfs_handle *h) {
  struct vfs_pipe *pipe = (struct vfs_pipe *)h->private_data;
  if (!pipe) return;
  while (__atomic_test_and_set(&pipe->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
  if (h->kind == VFS_HANDLE_PIPE_READ) {
    if (pipe->readers > 0) pipe->readers--;
  } else {
    if (pipe->writers > 0) pipe->writers--;
  }
  int free_pipe = (pipe->readers <= 0 && pipe->writers <= 0);
  char *doomed = 0;

  if (free_pipe) {
    /* Give the 64 KiB data buffer back beyond a small cache.
     *
     * Keeping it attached to the slot for ever is a fine trade while a machine
     * uses a handful of pipes, and a quarter of a gigabyte of pinned kernel
     * heap once a program has used a thousand of them: liburing's
     * poll-mshot-update opens exactly that many, and the boot that followed it
     * died in the page-table allocator with no memory left. The first
     * PIPE_BUFFER_CACHE slots keep theirs, so the churn the cache exists to
     * avoid is still avoided for everything but a burst. */
    if (__atomic_load_n(&g_pipe_buffers_held, __ATOMIC_RELAXED) >
        PIPE_BUFFER_CACHE) {
      doomed = pipe->buffer;
      pipe->buffer = 0;
    }
    pipe->used = 0;
  }
  __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
  if (doomed) {
    __atomic_sub_fetch(&g_pipe_buffers_held, 1, __ATOMIC_ACQ_REL);
    kfree(doomed); /* outside the pipe lock: kfree can grow the heap */
  }
  
  /* Wake up anyone waiting on the pipe */
  scheduler_wake_all(pipe);
  scheduler_wake_all(vfs_poll_chan);
}

/* FIONREAD: what a read would return right now, for either end.
 *
 * Qt asks it after poll(2) has said "readable" and, answered ENOTTY, takes
 * the count to be zero and reads nothing -- so the descriptor stays readable
 * and the event loop spins: kioworker made 110 000 poll+ioctl+clock_gettime
 * rounds a second while Plasma started, a third of the kernel's time. Every
 * other request is still ENOTTY, which isatty(3) relies on to mean "not a
 * terminal" rather than "not open" (see vfs_ioctl). */
#define PIPE_FIONREAD 0x541B
static int pipe_ioctl(struct vfs_handle *h, u64 request, void *arg) {
  if (request != PIPE_FIONREAD) return -ENOTTY;
  struct vfs_pipe *pipe = (struct vfs_pipe *)h->private_data;
  int n = (pipe && pipe->used) ? (int)pipe->size : 0;
  return syscall_copyout(arg, &n, sizeof(n)) == 0 ? 0 : -EFAULT;
}

const struct vfs_file_ops pipe_read_ops = { .read = pipe_read, .poll = pipe_poll, .release = pipe_release, .ioctl = pipe_ioctl };
const struct vfs_file_ops pipe_write_ops = { .write = pipe_write, .poll = pipe_poll, .release = pipe_release, .ioctl = pipe_ioctl };

void vfs_pipe_init_handle(struct vfs_handle *h, struct vfs_pipe *pipe, int is_write) {
  h->private_data = pipe;
  h->ops = is_write ? &pipe_write_ops : &pipe_read_ops;
  h->kind = is_write ? VFS_HANDLE_PIPE_WRITE : VFS_HANDLE_PIPE_READ;
  /* The access mode, because F_GETFL is asked and the answer is acted on: a
   * pipe end is read-only or write-only, never both. Leaving it zero made
   * fcntl(F_GETFL) report O_RDONLY for BOTH ends, and systemd — handed a pipe
   * by `systemd-run --pipe` — read that as a stdout it cannot write to and
   * refused the unit ("StandardOutputFileDescriptor passed is of incompatible
   * type"). The flag word is otherwise preserved, so O_NONBLOCK set later is
   * unaffected. */
  h->flags = (h->flags & ~(int)3) | (is_write ? B1NIX_O_WRONLY : B1NIX_O_RDONLY);
}

/* Atomically find a free pipes[] slot and CLAIM it before releasing the pool
 * lock, so no other CPU can pick the same slot. The caller does the full
 * memset() after the unlock — once `used = 1` is published, the slot is ours to
 * fill in. Returns NULL when the pool is exhausted. */
static struct vfs_pipe *pipe_pool_claim(void) {
  struct vfs_pipe *pipe = 0;
  u64 flags;
  spin_lock_irqsave(&pipe_pool_lock, &flags);
  for (usize i = 0; i < resource_caps_pipe_max(); i++) {
    if (!pipes[i].used) {
      pipe = &pipes[i];
      pipe->used = 1; /* claim atomically under the pool lock */
      break;
    }
  }
  spin_unlock_irqrestore(&pipe_pool_lock, flags);
  if (!pipe)
    return 0;
  /* The 64 KiB data buffer is attached to the slot on its first use and then
   * kept: a slot is only ever re-used as a pipe, so freeing and re-allocating
   * it per pipe() would just churn the heap. Allocated outside the pool lock —
   * kmalloc can grow the heap, which must not happen with a spinlock held. */
  if (!pipe->buffer) {
    pipe->buffer = kmalloc(PIPE_BUFFER_SIZE);
    if (pipe->buffer)
      __atomic_add_fetch(&g_pipe_buffers_held, 1, __ATOMIC_ACQ_REL);
    if (!pipe->buffer) {
      pipe->used = 0;
      return 0;
    }
  }
  return pipe;
}

/* memset() on a pool slot would drop the buffer pointer with it; this clears
 * the per-episode state and keeps the slot's buffer. */
static void pipe_slot_reset(struct vfs_pipe *p) {
  char *buf = p->buffer;
  memset(p, 0, sizeof(*p));
  p->buffer = buf;
}

int vfs_pipe(int pipefd[2]) {
  if (!pipefd) return -EINVAL;
  struct vfs_pipe *pipe = pipe_pool_claim();
  if (!pipe) return -ENFILE;

  struct vfs_handle *rh = alloc_raw_handle(VFS_HANDLE_PIPE_READ);
  if (!rh) { pipe->used = 0; return -EMFILE; }
  struct vfs_handle *wh = alloc_raw_handle(VFS_HANDLE_PIPE_WRITE);
  if (!wh) { vfs_handle_release(rh); pipe->used = 0; return -EMFILE; }

  /* The reset blows away `used=1` we just set; restore it. lock/refcount fields
   * are also re-zeroed which is correct — they start fresh for this episode. */
  pipe_slot_reset(pipe);
  pipe->used = 1;
  pipe->readers = 1;
  pipe->writers = 1;

  vfs_pipe_init_handle(rh, pipe, 0);
  vfs_pipe_init_handle(wh, pipe, 1);

  pipefd[0] = scheduler_fd_alloc(rh);
  pipefd[1] = scheduler_fd_alloc(wh);
  if (pipefd[0] < 0 || pipefd[1] < 0) {
    if (pipefd[0] >= 0) scheduler_fd_close(pipefd[0]);
    if (pipefd[1] >= 0) scheduler_fd_close(pipefd[1]);
    vfs_handle_release(rh);
    vfs_handle_release(wh);
    return -EMFILE;
  }
  return 0;
}

/* ── Named pipes (FIFOs) ──────────────────────────────────────────────────────
 * A FIFO is a VFS_FIFO node whose inode carries a struct vfs_pipe while it has
 * openers. The data path is the anonymous-pipe one — same buffer, same blocking
 * read/write/poll — so only the lifetime and the open-time rendezvous are new.
 *
 * Locking: a FIFO's readers/writers counts are mutated only under
 * fifo_attach_lock (open and release), never under pipe->lock, so attaching or
 * dropping inode->fifo is atomic with respect to the counts that decide it.
 * pipe_read/pipe_write only *read* those counts, exactly as they do for an
 * anonymous pipe. */
static spinlock_t fifo_attach_lock = SPINLOCK_INIT;

/* Drop this opener's reference to the FIFO. When the last reader and writer are
 * gone the buffer is detached from the inode and returned to the pool, so the
 * next open() starts with an empty FIFO (POSIX: no data survives a FIFO with no
 * openers). */
static void fifo_detach(struct vfs_inode *inode, struct vfs_pipe *fifo,
                        int was_reader, int was_writer) {
  u64 irq;
  spin_lock_irqsave(&fifo_attach_lock, &irq);
  if (was_reader && fifo->readers > 0)
    fifo->readers--;
  if (was_writer && fifo->writers > 0)
    fifo->writers--;
  if (fifo->readers <= 0 && fifo->writers <= 0) {
    if (inode && inode->fifo == fifo)
      inode->fifo = 0;
    fifo->used = 0;
  }
  spin_unlock_irqrestore(&fifo_attach_lock, irq);

  /* A peer blocked in read()/write() must observe the closed end: a reader sees
   * writers == 0 and returns EOF, a writer sees readers == 0 and gets EPIPE. */
  scheduler_wake_all(fifo);
  scheduler_wake_all(vfs_poll_chan);
}

static void fifo_release(struct vfs_handle *h) {
  struct vfs_pipe *fifo = (struct vfs_pipe *)h->private_data;
  struct vfs_node *node = h->node;
  if (fifo) {
    int acc = h->flags & 3;
    int was_reader = (acc == B1NIX_O_RDONLY) || (acc == B1NIX_O_RDWR);
    int was_writer = (acc == B1NIX_O_WRONLY) || (acc == B1NIX_O_RDWR);
    fifo_detach(node ? node->inode : 0, fifo, was_reader, was_writer);
  }
  if (node)
    vfs_node_put(node);
}

static const struct vfs_file_ops fifo_read_ops = {
    .read = pipe_read, .poll = pipe_poll, .release = fifo_release, .ioctl = pipe_ioctl};
static const struct vfs_file_ops fifo_write_ops = {
    .write = pipe_write, .poll = pipe_poll, .release = fifo_release};
/* O_RDWR on a FIFO is legal on Linux and never blocks — the opener is its own
 * peer, so both directions are wired up on one handle. */
static const struct vfs_file_ops fifo_rdwr_ops = {
    .read = pipe_read, .write = pipe_write, .poll = pipe_poll,
    .release = fifo_release};

int vfs_fifo_open(struct vfs_node *node, int flags) {
  if (!node || !node->inode)
    return -EINVAL;
  int acc = flags & 3;
  int want_read = (acc == B1NIX_O_RDONLY) || (acc == B1NIX_O_RDWR);
  int want_write = (acc == B1NIX_O_WRONLY) || (acc == B1NIX_O_RDWR);
  if (!want_read && !want_write)
    return -EINVAL;

  /* Claim a pool slot up front: pipe_pool_claim takes pipe_pool_lock, which
   * must not nest inside fifo_attach_lock. The spare is released again if this
   * inode already has a buffer. */
  struct vfs_pipe *spare = pipe_pool_claim();
  if (!spare)
    return -ENFILE;

  u64 irq;
  spin_lock_irqsave(&fifo_attach_lock, &irq);
  struct vfs_pipe *fifo = node->inode->fifo;
  if (!fifo) {
    pipe_slot_reset(spare);
    spare->used = 1;
    fifo = spare;
    node->inode->fifo = fifo;
    spare = 0;
  }
  /* POSIX: O_WRONLY | O_NONBLOCK with no reader fails outright, and must not
   * register a writer (that would fake a peer for a later reader). */
  if (want_write && !want_read && (flags & B1NIX_O_NONBLOCK) &&
      fifo->readers == 0) {
    int empty = (fifo->readers <= 0 && fifo->writers <= 0);
    if (empty) {
      node->inode->fifo = 0;
      fifo->used = 0;
    }
    spin_unlock_irqrestore(&fifo_attach_lock, irq);
    if (spare)
      spare->used = 0;
    return -ENXIO;
  }
  if (want_read) {
    fifo->readers++;
    fifo->reader_opens++;
  }
  if (want_write) {
    fifo->writers++;
    fifo->writer_opens++;
  }
  spin_unlock_irqrestore(&fifo_attach_lock, irq);
  if (spare)
    spare->used = 0;

  /* Publish this end to a peer already blocked in its own open(). */
  scheduler_wake_all(fifo);

  /* Rendezvous: a blocking open waits for the opposite end. O_RDWR is its own
   * peer and never waits. */
  if (!(flags & B1NIX_O_NONBLOCK) && acc != B1NIX_O_RDWR) {
    while (want_read ? fifo->writer_opens == 0 : fifo->reader_opens == 0) {
      if (scheduler_signal_pending()) {
        fifo_detach(node->inode, fifo, want_read, want_write);
        return -ERESTARTSYS;
      }
      interrupts_disable();
      current_task->wait_chan = fifo;
      scheduler_lease_clear_here(__func__);
      current_task->state = TASK_BLOCKED;
      scheduler_yield();
      interrupts_enable();
    }
  }

  struct vfs_handle *h = alloc_raw_handle(
      want_read ? VFS_HANDLE_PIPE_READ : VFS_HANDLE_PIPE_WRITE);
  if (!h) {
    fifo_detach(node->inode, fifo, want_read, want_write);
    return -ENFILE;
  }
  h->node = vfs_node_get(node);
  h->private_data = fifo;
  h->flags = flags;
  /* A pipe has no file position; a FIFO's read end keeps the writer count it
   * was opened at, for pipe_poll. */
  h->offset = fifo->writer_opens;
  h->ops = (acc == B1NIX_O_RDWR)  ? &fifo_rdwr_ops
           : want_write           ? &fifo_write_ops
                                  : &fifo_read_ops;

  int fd = scheduler_fd_alloc(h);
  if (fd < 0) {
    vfs_handle_release(h); /* runs fifo_release: detach + node put */
    return -EMFILE;
  }
  if (flags & B1NIX_O_CLOEXEC)
    scheduler_fd_flags_set(fd, B1NIX_FD_CLOEXEC);
  return fd;
}
