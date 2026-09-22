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
/* Hand `nr` wakes to the FUTEX_WAIT requests parked on this key; returns how
 * many were served. Called from the futex layer's wake path. */
int io_uring_futex_wake(u64 key_pml4, u64 key_word, int nr);
/* Bring the IORING_SQ_TASKRUN flag of this task's deferred rings up to date. */
void io_uring_taskrun_refresh(void);
/* Print what every ring is waiting for; called from the watchdog dump. */
void io_uring_dump_state(void);

#endif /* B1NIX_IO_URING_H */
