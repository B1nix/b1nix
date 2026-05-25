#ifndef B1NIX_U_AIO_H
#define B1NIX_U_AIO_H

#include <types.h>

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

#endif
