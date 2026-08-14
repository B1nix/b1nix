/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_COMPAT_H
#define LKPI_LINUX_COMPAT_H
#include <linux/types.h>
/* 32-bit userspace on a 64-bit kernel. b1nix is x86_64-only and its 32-bit port
 * is frozen, so there is no compat ABI to translate and the types exist only
 * for the ioctl tables that mention them. */
typedef u32 compat_uptr_t;
typedef u32 compat_size_t;
typedef s32 compat_int_t;
typedef u32 compat_uint_t;
typedef u64 compat_u64;
static inline void *compat_ptr(compat_uptr_t uptr) { return (void *)(usize)uptr; }
static inline int in_compat_syscall(void) { return 0; }
#endif
