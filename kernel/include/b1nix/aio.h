#ifndef B1NIX_AIO_H
#define B1NIX_AIO_H

#include <b1nix/types.h>

struct task;

#define B1NIX_AIO_OP_READ  1
#define B1NIX_AIO_OP_WRITE 2

struct b1nix_aio_sqe {
  u64 user_data;
  int fd;
  u16 opcode;
  u16 flags;
  u64 offset;
  u64 addr;
  u32 len;
  u32 reserved;
};

struct b1nix_aio_cqe {
  u64 user_data;
  isize res;
  u32 flags;
  u32 reserved;
};

void aio_init(void);
void aio_task_cleanup(struct task *task);

u64 sys_io_setup(u32 entries, u64 *user_ctx);
u64 sys_io_submit(u64 ctx_id, u32 nr, const struct b1nix_aio_sqe *user_sqes);
u64 sys_io_getevents(u64 ctx_id, u32 min_nr, u32 max_nr,
                     struct b1nix_aio_cqe *user_cqes, u32 timeout_ticks);

#endif
