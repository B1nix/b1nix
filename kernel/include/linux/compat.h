/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_COMPAT_H
#define LKPI_LINUX_COMPAT_H
#include <linux/types.h>
/* 32-bit userspace on a 64-bit kernel. b1nix runs no 32-bit userspace, so
 * there is no compat ABI to translate and the types exist only
 * for the ioctl tables that mention them. */
typedef u32 compat_uptr_t;
typedef u32 compat_size_t;
typedef s32 compat_int_t;
typedef u32 compat_uint_t;
typedef u64 compat_u64;
static inline void *compat_ptr(compat_uptr_t uptr) { return (void *)(usize)uptr; }
static inline int in_compat_syscall(void) { return 0; }
/* No 32-bit tasks, so never the i386 64-bit alignment of a compat structure. */
static inline bool compat_need_64bit_alignment_fixup(void) { return false; }
#endif
