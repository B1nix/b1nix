#ifndef B1NIX_U_AIO_H
#define B1NIX_U_AIO_H

/* Standard widths, not our own aliases.
 *
 * This included <types.h> from the parallel libc header tree that used to sit
 * beside it. That tree is gone -- everything here compiles against musl's
 * headers now -- and the layout has to be spelled out in types musl knows.
 * The widths are unchanged, because this describes a structure the kernel
 * reads: kernel/fs/aio.c must see exactly the same bytes. */
#include <stdint.h>
#include <sys/types.h>

#define B1NIX_AIO_OP_READ  1
#define B1NIX_AIO_OP_WRITE 2

struct b1nix_aio_sqe {
  uint64_t user_data;
  int fd;
  uint16_t opcode;
  uint16_t flags;
  uint64_t offset;
  uint64_t addr;
  uint32_t len;
  uint32_t reserved;
};

struct b1nix_aio_cqe {
  uint64_t user_data;
  ssize_t res;
  uint32_t flags;
  uint32_t reserved;
};

#endif
