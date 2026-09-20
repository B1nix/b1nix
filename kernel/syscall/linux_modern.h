/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_SYSCALL_LINUX_MODERN_H
#define B1NIX_SYSCALL_LINUX_MODERN_H
#include <b1nix/types.h>

/* Handle a Linux system call added after the ABI table was built; returns 1
 * and sets *ret when `nr` is one of them. See linux_modern.c. */
int linux_modern_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                         u64 a5, u64 *ret);
void linux_modern_task_reset(usize slot);
void linux_modern_fork_inherit(usize parent_slot, usize child_slot);
/* add_key/request_key/keyctl; see linux_keys.c. */
int linux_keys_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                       u64 *ret);
void linux_keys_task_reset(usize row);
void linux_keys_fork_inherit(usize parent_row, usize child_row);

/* From syscall.c, where the implementations live. */
isize linux_modern_pkey_mprotect(u64 addr, u64 len, u64 prot, u64 pkey);
int linux_modern_open_flags(int linux_flags);
isize linux_modern_remap_file_pages(u64 start, u64 size, u64 prot, u64 pgoff,
                                    u64 flags);

/* futex_waitv: one entry per futex. */
struct scheduler_futex_vec {
  u64 uaddr;
  int val;
  int priv;
};
int scheduler_futex_waitv(struct scheduler_futex_vec *v, int n, u64 timeout_ms);
#endif
