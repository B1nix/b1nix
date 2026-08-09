/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_TYPES_H
#define LKPI_LINUX_TYPES_H

#include <b1nix/types.h>
#include <linux/compiler.h>
#include <linux/stddef.h>

/* The __-prefixed spellings the uapi headers use. b1nix's own u8/u32/... come
 * from <b1nix/types.h> and are the same underlying types. */
typedef signed char __s8;
typedef unsigned char __u8;
typedef short __s16;
typedef unsigned short __u16;
typedef int __s32;
typedef unsigned int __u32;
typedef i64 __s64;
/* Spelled as b1nix's u64 rather than `unsigned long long`. Both are 64 bits so
 * every struct layout is identical, but a *pointer* to one is not
 * interchangeable with a pointer to the other, and imported code passes
 * &args->field into helpers typed with the other spelling. One 64-bit type
 * keeps those compatible. */
typedef u64 __u64;

typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;
typedef __u64 __le64;
typedef __u64 __be64;

/* Linux's signed spellings. b1nix names these i8/i16/i32/i64; same types. */
typedef i8 s8;
typedef i16 s16;
typedef i32 s32;
typedef i64 s64;

typedef unsigned long ulong;
typedef unsigned int uint;
typedef unsigned int __poll_t;

/* Page protection bits, as a struct so a raw integer cannot be passed where
 * one belongs — the same reason atomic_t is a struct. */
typedef struct { u64 pgprot; } pgprot_t;
#define pgprot_val(v) ((v).pgprot)
#define __pgprot(v)   ((pgprot_t){ (v) })
typedef _Bool bool;
#define true 1
#define false 0

typedef unsigned short umode_t;
typedef u64 resource_size_t;
typedef unsigned int gfp_t;
typedef int atomic_t_placeholder;
typedef unsigned int fmode_t;

/* The __kernel_-prefixed spellings the uapi headers use for sizes and ids. */
typedef unsigned long __kernel_size_t;
typedef long __kernel_ssize_t;
typedef long __kernel_ptrdiff_t;
typedef long long __kernel_loff_t;
typedef int __kernel_pid_t;
typedef unsigned int __kernel_uid32_t;
typedef unsigned int __kernel_gid32_t;
typedef __kernel_size_t size_t;
typedef __kernel_ssize_t ssize_t;
typedef __kernel_loff_t loff_t;
typedef unsigned long pgoff_t;
typedef int pid_t;
typedef u64 phys_addr_t;

/* Alignment a DMA buffer must have for cache maintenance to be safe on this
 * architecture. x86 is cache-coherent for DMA, so the constraint is only the
 * cache line. */
#define ARCH_DMA_MINALIGN 64

/* Pulled in here because imported headers name wait_queue_head_t without
 * including <linux/wait.h>, relying on Linux's own transitive includes. */
#include <lkpi/wait.h>

/* ktime_t, memset/memcpy and the WARN family are named by imported headers that
 * do not include <linux/ktime.h>, <linux/string.h> or <linux/bug.h> — on Linux
 * some other header they include drags each in. Pulled here, the one header
 * everything includes, rather than guessing which. */
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/bug.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/overflow.h>
#include <lkpi/rcu.h>
#include <linux/fcntl.h>
#include <linux/capability.h>
#include <linux/stringify.h>
#include <linux/io.h>
#include <linux/sysfs.h>
#include <linux/pid.h>
#include <linux/preempt.h>
#include <linux/kstrtox.h>
#include <linux/errno.h>
#include <linux/byteorder.h>
#include <linux/refcount.h>
#include <linux/dma-mapping.h>
#include <linux/kdev_t.h>
#include <linux/string_helpers.h>
#include <linux/poll.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/sched.h>

/* The calling task, as much of it as imported code reads. Here rather than in
 * <linux/sched.h> because files name `current` without including that. */
#include <lkpi/env.h>
#define task_struct lkpi_task
#ifndef current
#define current (lkpi_current())
#endif
#include <linux/time64.h>



#endif
