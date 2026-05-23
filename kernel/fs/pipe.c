#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <stdlib.h>
#include <string.h>

struct vfs_pipe pipes[MAX_VFS_PIPES];

static isize pipe_read(struct vfs_handle *h, char *buf, usize size) {
  struct vfs_pipe *pipe = (struct vfs_pipe *)h->private_data;
  if (!pipe || !pipe->used) return -EIO;
  
  while (1) {
    while (__atomic_test_and_set(&pipe->lock, __ATOMIC_ACQUIRE)) scheduler_yield();
    if (pipe->size == 0) {
      if (pipe->writers == 0) { __atomic_clear(&pipe->lock, __ATOMIC_RELEASE); return 0; }
      if (h->flags & B1NIX_O_NONBLOCK) { __atomic_clear(&pipe->lock, __ATOMIC_RELEASE); return -EAGAIN; }
      __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
      scheduler_block_on(pipe);
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
      __atomic_clear(&pipe->lock, __ATOMIC_RELEASE);
      scheduler_block_on(pipe);
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
  for (usize i = 0; i < MAX_VFS_PIPES; i++) {
    if (!pipes[i].used) { pipe = &pipes[i]; break; }
  }
  if (!pipe) return -ENFILE;

  int rfd_h = alloc_raw_handle(VFS_HANDLE_PIPE_READ);
  if (rfd_h < 0) return -EMFILE;
  int wfd_h = alloc_raw_handle(VFS_HANDLE_PIPE_WRITE);
  if (wfd_h < 0) { release_handle(rfd_h); return -EMFILE; }

  memset(pipe, 0, sizeof(*pipe));
  pipe->used = 1;
  pipe->readers = 1;
  pipe->writers = 1;

  struct vfs_handle *rh = get_handle_by_idx(rfd_h);
  struct vfs_handle *wh = get_handle_by_idx(wfd_h);
  vfs_pipe_init_handle(rh, pipe, 0);
  vfs_pipe_init_handle(wh, pipe, 1);

  pipefd[0] = scheduler_fd_alloc(rfd_h);
  pipefd[1] = scheduler_fd_alloc(wfd_h);
  if (pipefd[0] < 0 || pipefd[1] < 0) {
    if (pipefd[0] >= 0) scheduler_fd_close(pipefd[0]);
    if (pipefd[1] >= 0) scheduler_fd_close(pipefd[1]);
    release_handle(rfd_h);
    release_handle(wfd_h);
    return -EMFILE;
  }
  return 0;
}
