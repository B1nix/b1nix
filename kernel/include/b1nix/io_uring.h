/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_IO_URING_H
#define B1NIX_IO_URING_H

#include <b1nix/io_uring_abi.h>
#include <b1nix/types.h>

/* io_uring_setup(2) / io_uring_enter(2) / io_uring_register(2), Linux numbers
 * 425-427 on every architecture. Returns 1 and sets *ret when `nr` is one of
 * them, 0 otherwise — the shape linux_modern_syscall and landlock_syscall use.
 * See kernel/fs/io_uring.c. */
int io_uring_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
                     u64 *ret);

/* The futex layer telling io_uring that a word was woken, so a ring with a
 * FUTEX_WAIT armed re-checks it now rather than at its next event. Cheap when
 * nothing is waiting: one relaxed load. */
void io_uring_futex_hint(void);

#endif /* B1NIX_IO_URING_H */
