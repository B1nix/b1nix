#include <b1nix/aio.h>
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/syscall.h>
#include <b1nix/vfs.h>
#include <string.h>

#define AIO_MAX_ENTRIES 128
#define AIO_MAX_IO_SIZE (1024 * 1024)

struct aio_request {
  struct aio_request *next;
  struct task *owner;
  struct vfs_handle *handle;
  struct b1nix_aio_sqe sqe;
  char *kbuf;
  isize result;
};

struct aio_context {
  struct task *owner;
  u32 entries;
  u32 inflight;
  u32 cq_head;
  u32 cq_tail;
  struct aio_request **completed;
  volatile int lock;
  struct aio_context *next;
};

static struct aio_request *aio_pending_head = 0;
static struct aio_request *aio_pending_tail = 0;
static volatile int aio_pending_lock = 0;
static char aio_worker_chan;
static int aio_worker_created = 0;
static int aio_worker_started = 0;
static struct aio_context *aio_ctx_list = 0;
static volatile int aio_ctx_list_lock = 0;

static void aio_ctx_list_acquire(void) {
  while (__atomic_test_and_set(&aio_ctx_list_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
}

static void aio_ctx_list_release(void) {
  __atomic_clear(&aio_ctx_list_lock, __ATOMIC_RELEASE);
}

static struct aio_context *aio_ctx_find_by_task(struct task *owner) {
  aio_ctx_list_acquire();
  struct aio_context *it = aio_ctx_list;
  while (it) {
    if (it->owner == owner) {
      aio_ctx_list_release();
      return it;
    }
    it = it->next;
  }
  aio_ctx_list_release();
  return 0;
}

static void aio_ctx_list_add(struct aio_context *ctx) {
  aio_ctx_list_acquire();
  ctx->next = aio_ctx_list;
  aio_ctx_list = ctx;
  aio_ctx_list_release();
}

static void aio_ctx_list_remove(struct aio_context *ctx) {
  aio_ctx_list_acquire();
  struct aio_context **pp = &aio_ctx_list;
  while (*pp) {
    if (*pp == ctx) {
      *pp = ctx->next;
      break;
    }
    pp = &(*pp)->next;
  }
  aio_ctx_list_release();
}

static void aio_ctx_acquire(struct aio_context *ctx) {
  while (__atomic_test_and_set(&ctx->lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
}

static void aio_ctx_release(struct aio_context *ctx) {
  __atomic_clear(&ctx->lock, __ATOMIC_RELEASE);
}

static void aio_pending_acquire(void) {
  while (__atomic_test_and_set(&aio_pending_lock, __ATOMIC_ACQUIRE))
    scheduler_yield();
}

static void aio_pending_release(void) {
  __atomic_clear(&aio_pending_lock, __ATOMIC_RELEASE);
}

static u32 aio_cq_count_locked(const struct aio_context *ctx) {
  if (ctx->cq_tail >= ctx->cq_head)
    return ctx->cq_tail - ctx->cq_head;
  return ctx->entries - (ctx->cq_head - ctx->cq_tail);
}

static void aio_enqueue_pending(struct aio_request *req) {
  req->next = 0;
  aio_pending_acquire();
  if (!aio_pending_tail) {
    aio_pending_head = req;
    aio_pending_tail = req;
  } else {
    aio_pending_tail->next = req;
    aio_pending_tail = req;
  }
  aio_pending_release();
  scheduler_wake_all(&aio_worker_chan);
}

static struct aio_request *aio_dequeue_pending(void) {
  aio_pending_acquire();
  struct aio_request *req = aio_pending_head;
  if (req) {
    aio_pending_head = req->next;
    if (!aio_pending_head)
      aio_pending_tail = 0;
    req->next = 0;
  }
  aio_pending_release();
  return req;
}

static void aio_complete_request(struct aio_request *req) {
  struct aio_context *ctx = aio_ctx_find_by_task(req->owner);
  if (!ctx || ctx->owner != req->owner) {
    if (req->kbuf)
      kfree(req->kbuf);
    vfs_handle_release(req->handle);
    kfree(req);
    return;
  }

  aio_ctx_acquire(ctx);
  u32 next_tail = (ctx->cq_tail + 1) % ctx->entries;
  if (next_tail == ctx->cq_head) {
    /* CQ overflow should not happen due to inflight bound; drop oldest safely. */
    struct aio_request *old = ctx->completed[ctx->cq_head];
    if (old) {
      if (old->kbuf)
        kfree(old->kbuf);
      vfs_handle_release(old->handle);
      kfree(old);
    }
    ctx->cq_head = (ctx->cq_head + 1) % ctx->entries;
    if (ctx->inflight > 0)
      ctx->inflight--;
  }
  ctx->completed[ctx->cq_tail] = req;
  ctx->cq_tail = next_tail;
  aio_ctx_release(ctx);
  scheduler_wake_all((void *)ctx);
}

static void aio_worker_thread(void *arg) {
  (void)arg;
  aio_worker_started = 1;
  for (;;) {
    struct aio_request *req = aio_dequeue_pending();
    if (!req) {
      scheduler_block_on(&aio_worker_chan);
      continue;
    }

    isize res = -EINVAL;
    if (!req->handle || !req->handle->ops) {
      res = -EBADF;
    } else if (req->sqe.opcode == B1NIX_AIO_OP_READ) {
      if (!req->handle->ops->read || !req->kbuf) {
        res = -EINVAL;
      } else {
        /* Use a local handle copy so AIO offset never races on shared fd state. */
        struct vfs_handle local = *req->handle;
        local.offset = (usize)req->sqe.offset;
        res = local.ops->read(&local, req->kbuf, req->sqe.len);
      }
    } else if (req->sqe.opcode == B1NIX_AIO_OP_WRITE) {
      if (!req->handle->ops->write || !req->kbuf) {
        res = -EINVAL;
      } else {
        /* Use a local handle copy so AIO offset never races on shared fd state. */
        struct vfs_handle local = *req->handle;
        local.offset = (usize)req->sqe.offset;
        res = local.ops->write(&local, req->kbuf, req->sqe.len);
      }
    }
    req->result = res;
    aio_complete_request(req);
  }
}

void aio_init(void) {
  if (aio_worker_created)
    return;
  aio_worker_created = 1;
  if (kthread_create("aio-worker", aio_worker_thread, 0) < 0)
    aio_worker_created = 0;
}

void aio_task_cleanup(struct task *task) {
  if (!task)
    return;
  struct aio_context *ctx = aio_ctx_find_by_task(task);
  if (!ctx)
    return;
  aio_ctx_list_remove(ctx);
  aio_ctx_acquire(ctx);
  for (u32 i = 0; i < ctx->entries; i++) {
    struct aio_request *req = ctx->completed[i];
    if (!req)
      continue;
    ctx->completed[i] = 0;
    if (req->kbuf)
      kfree(req->kbuf);
    vfs_handle_release(req->handle);
    kfree(req);
  }
  aio_ctx_release(ctx);
  kfree(ctx->completed);
  kfree(ctx);
}

int vfs_submit_aio(struct task *owner, const struct b1nix_aio_sqe *sqe) {
  if (!owner || !sqe)
    return -EINVAL;
  struct aio_context *ctx = aio_ctx_find_by_task(owner);
  if (!ctx || ctx->owner != owner)
    return -EINVAL;

  if (sqe->opcode != B1NIX_AIO_OP_READ && sqe->opcode != B1NIX_AIO_OP_WRITE)
    return -EINVAL;
  if (sqe->len == 0)
    return 0;
  if (sqe->len > AIO_MAX_IO_SIZE)
    return -E2BIG;

  struct vfs_handle *h = 0;
  if (sqe->fd >= 0 && owner->fd_table && (usize)sqe->fd < owner->fd_capacity) {
    h = owner->fd_table[sqe->fd];
  }
  if (!h)
    return -EBADF;

  struct aio_request *req = kzalloc(sizeof(struct aio_request));
  if (!req)
    return -ENOMEM;
  req->owner = owner;
  req->handle = h;
  req->sqe = *sqe;
  req->result = -EAGAIN;

  req->kbuf = kmalloc(sqe->len);
  if (!req->kbuf) {
    kfree(req);
    return -ENOMEM;
  }

  if (sqe->opcode == B1NIX_AIO_OP_WRITE) {
    int rc = syscall_copyin(req->kbuf, (const void *)(usize)sqe->addr, sqe->len);
    if (rc < 0) {
      kfree(req->kbuf);
      kfree(req);
      return rc;
    }
  } else {
    memset(req->kbuf, 0, sqe->len);
  }

  aio_ctx_acquire(ctx);
  if (ctx->inflight >= ctx->entries) {
    aio_ctx_release(ctx);
    kfree(req->kbuf);
    kfree(req);
    return -EAGAIN;
  }
  ctx->inflight++;
  aio_ctx_release(ctx);

  vfs_handle_retain(h);
  aio_enqueue_pending(req);
  return 1;
}

u64 sys_io_setup(u32 entries, u64 *user_ctx) {
  aio_init();
  if (!user_ctx)
    return (u64)-EFAULT;
  if (!current_task)
    return (u64)-EINVAL;
  if (aio_ctx_find_by_task(current_task))
    return (u64)-EEXIST;
  if (entries == 0)
    entries = 32;
  if (entries > AIO_MAX_ENTRIES)
    entries = AIO_MAX_ENTRIES;

  struct aio_context *ctx = kzalloc(sizeof(struct aio_context));
  if (!ctx)
    return (u64)-ENOMEM;
  ctx->owner = current_task;
  ctx->entries = entries;
  ctx->completed = kzalloc(sizeof(struct aio_request *) * entries);
  if (!ctx->completed) {
    kfree(ctx);
    return (u64)-ENOMEM;
  }
  aio_ctx_list_add(ctx);

  u64 id = (u64)(usize)ctx;
  if (syscall_copyout(user_ctx, &id, sizeof(id)) < 0) {
    aio_task_cleanup(current_task);
    return (u64)-EFAULT;
  }
  return 0;
}

u64 sys_io_submit(u64 ctx_id, u32 nr, const struct b1nix_aio_sqe *user_sqes) {
  if (!current_task)
    return (u64)-EINVAL;
  struct aio_context *ctx = aio_ctx_find_by_task(current_task);
  if (!ctx)
    return (u64)-EINVAL;
  if ((u64)(usize)ctx != ctx_id)
    return (u64)-EINVAL;
  if (nr == 0 || !user_sqes)
    return 0;

  u32 submitted = 0;
  for (u32 i = 0; i < nr; i++) {
    struct b1nix_aio_sqe sqe;
    if (syscall_copyin(&sqe, &user_sqes[i], sizeof(sqe)) < 0) {
      if (submitted == 0)
        return (u64)-EFAULT;
      return submitted;
    }
    int rc = vfs_submit_aio(current_task, &sqe);
    if (rc < 0) {
      if (submitted == 0)
        return (u64)rc;
      return submitted;
    }
    submitted++;
  }
  return submitted;
}

u64 sys_io_getevents(u64 ctx_id, u32 min_nr, u32 max_nr,
                     struct b1nix_aio_cqe *user_cqes, u32 timeout_ticks) {
  if (!current_task)
    return (u64)-EINVAL;
  struct aio_context *ctx = aio_ctx_find_by_task(current_task);
  if (!ctx)
    return (u64)-EINVAL;
  if ((u64)(usize)ctx != ctx_id)
    return (u64)-EINVAL;
  if (max_nr == 0 || !user_cqes)
    return 0;
  if (min_nr > max_nr)
    return (u64)-EINVAL;

  u64 start = scheduler_get_uptime_ticks();
  for (;;) {
    aio_ctx_acquire(ctx);
    u32 available = aio_cq_count_locked(ctx);
    aio_ctx_release(ctx);
    if (available >= min_nr)
      break;
    if (timeout_ticks == 0)
      break;
    if (scheduler_get_uptime_ticks() - start >= timeout_ticks)
      break;
    scheduler_block_on((void *)ctx);
  }

  u32 got = 0;
  while (got < max_nr) {
    aio_ctx_acquire(ctx);
    if (ctx->cq_head == ctx->cq_tail) {
      aio_ctx_release(ctx);
      break;
    }
    struct aio_request *req = ctx->completed[ctx->cq_head];
    ctx->completed[ctx->cq_head] = 0;
    ctx->cq_head = (ctx->cq_head + 1) % ctx->entries;
    if (ctx->inflight > 0)
      ctx->inflight--;
    aio_ctx_release(ctx);

    struct b1nix_aio_cqe cqe;
    cqe.user_data = req->sqe.user_data;
    cqe.flags = 0;
    cqe.reserved = 0;
    cqe.res = req->result;

    if (req->sqe.opcode == B1NIX_AIO_OP_READ && req->result > 0) {
      int rc = syscall_copyout((void *)(usize)req->sqe.addr, req->kbuf,
                               (usize)req->result);
      if (rc < 0)
        cqe.res = rc;
    }

    if (syscall_copyout(&user_cqes[got], &cqe, sizeof(cqe)) < 0) {
      if (req->kbuf)
        kfree(req->kbuf);
      vfs_handle_release(req->handle);
      kfree(req);
      return got > 0 ? got : (u64)-EFAULT;
    }

    if (req->kbuf)
      kfree(req->kbuf);
    vfs_handle_release(req->handle);
    kfree(req);
    got++;
  }

  return got;
}
