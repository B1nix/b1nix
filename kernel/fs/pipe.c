#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/arch.h>
#include <b1nix/spinlock.h>
#include <stdlib.h>
#include <string.h>

struct vfs_pipe pipes[MAX_VFS_PIPES];
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

static isize pipe_read(struct vfs_handle *h, char *buf, usize size) {
  struct vfs_pipe *pipe = (struct vfs_pipe *)h->private_data;
  if (!pipe || !pipe->used) return -EIO;
  
  while (1) {
    while (__atomic_test_and_set(&pipe->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
    if (pipe->size == 0) {
      if (pipe->writers == 0) { __atomic_clear(&pipe->lock, __ATOMIC_RELEASE); return 0; }
      if (h->flags & B1NIX_O_NONBLOCK) { __atomic_clear(&pipe->lock, __ATOMIC_RELEASE); return -EAGAIN; }
      interrupts_disable();
      current_task->wait_chan = pipe;
      current_task->stack_released = 0;
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
      interrupts_disable();
      current_task->wait_chan = pipe;
      current_task->stack_released = 0;
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

static int pipe_poll(struct vfs_handle *h, struct b1nix_pollfd *pfd) {
  struct vfs_pipe *pipe = (struct vfs_pipe *)h->private_data;
  if (!pipe || !pipe->used) { pfd->revents = B1NIX_POLLHUP; return 0; }
  pfd->revents = 0;
  if (h->kind == VFS_HANDLE_PIPE_READ) {
    if (pipe->size > 0) pfd->revents |= B1NIX_POLLIN;
    if (pipe->writers == 0) pfd->revents |= B1NIX_POLLHUP;
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
  if (free_pipe) pipe->used = 0;
  __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
  
  /* Wake up anyone waiting on the pipe */
  scheduler_wake_all(pipe);
  scheduler_wake_all(vfs_poll_chan);
}

const struct vfs_file_ops pipe_read_ops = { .read = pipe_read, .poll = pipe_poll, .release = pipe_release };
const struct vfs_file_ops pipe_write_ops = { .write = pipe_write, .poll = pipe_poll, .release = pipe_release };

void vfs_pipe_init_handle(struct vfs_handle *h, struct vfs_pipe *pipe, int is_write) {
  h->private_data = pipe;
  h->ops = is_write ? &pipe_write_ops : &pipe_read_ops;
  h->kind = is_write ? VFS_HANDLE_PIPE_WRITE : VFS_HANDLE_PIPE_READ;
}

int vfs_pipe(int pipefd[2]) {
  if (!pipefd) return -EINVAL;
  struct vfs_pipe *pipe = 0;
  /* Atomically find a free slot and CLAIM it before releasing the pool lock,
   * so no other CPU can pick the same slot. The full memset() happens after
   * the unlock — once `used = 1` is published, the slot is ours to fill in. */
  u64 flags;
  spin_lock_irqsave(&pipe_pool_lock, &flags);
  for (usize i = 0; i < MAX_VFS_PIPES; i++) {
    if (!pipes[i].used) {
      pipe = &pipes[i];
      pipe->used = 1;  /* claim atomically under the pool lock */
      break;
    }
  }
  spin_unlock_irqrestore(&pipe_pool_lock, flags);
  if (!pipe) return -ENFILE;

  struct vfs_handle *rh = alloc_raw_handle(VFS_HANDLE_PIPE_READ);
  if (!rh) { pipe->used = 0; return -EMFILE; }
  struct vfs_handle *wh = alloc_raw_handle(VFS_HANDLE_PIPE_WRITE);
  if (!wh) { vfs_handle_release(rh); pipe->used = 0; return -EMFILE; }

  /* memset() blows away `used=1` we just set; restore it. lock/refcount fields
   * are also re-zeroed which is correct — they start fresh for this episode. */
  memset(pipe, 0, sizeof(*pipe));
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
