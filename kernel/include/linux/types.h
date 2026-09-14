/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TYPES_H
#define LKPI_LINUX_TYPES_H

#include <b1nix/types.h>
#include <linux/compiler.h>
/* The memory barriers. Reached from here because <linux/types.h> is what every
 * imported translation unit force-includes, and imported code uses smp_rmb()
 * from headers that include neither <linux/smp.h> nor <linux/atomic.h>. */
#include <asm/barrier.h>
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
/* The userspace spelling of the same thing, which ext4 uses in an internal
 * helper's signature. */
typedef unsigned int mode_t;
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

/*
 * Filesystem and block-layer scalars.
 *
 * `sector_t` is 64-bit unconditionally. Upstream makes it depend on
 * CONFIG_LBDAF on 32-bit machines; here it never does, because a 32-bit
 * sector_t caps a device at 2 TiB and silently wraps past it — and the wrap is
 * not an error, it is a write to the wrong place.
 *
 * `blkcnt_t` counts blocks of a filesystem's own size, not sectors. They are
 * different units and the distinction is what `i_blocks` (always 512-byte
 * units) versus `i_blkbits` exists to keep straight.
 */
typedef u64 sector_t;
typedef u64 blkcnt_t;
#ifndef LKPI_TIME64_T_DEFINED
#define LKPI_TIME64_T_DEFINED
typedef long long time64_t;
#endif
typedef unsigned int uid_t;
typedef unsigned int gid_t;
/* The third id class, used by the quota code for project quotas. */
typedef unsigned int projid_t;
typedef unsigned int dev_t;
/* pfn_t is defined in <linux/pfn_t.h>, which is where the DAX interfaces that
 * use it look for it; it was briefly defined here too, which is a redefinition
 * rather than a second spelling. */

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
/* Late, not with the scalars above: it defines kuid_t/kgid_t as structs over
 * uid_t/gid_t, so those typedefs have to be in scope first. */
#include <linux/uidgid.h>
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
/*
 * NOT <linux/poll.h>.
 *
 * It includes <linux/fs.h>, and this header is reached from
 * <linux/spinlock.h> — so pulling fs.h in here means fs.h is compiled before
 * `spinlock_t` exists, and every lock member in it is an unknown type. It was
 * harmless while fs.h was a handful of anonymous-inode declarations; it stopped
 * being harmless when fs.h became the VFS. A driver that needs poll includes it
 * itself, which is what upstream expects anyway.
 *
 * What that include was quietly providing, besides poll: file-scope
 * declarations of the VFS types. Imported headers name them inside function
 * POINTER members, where a first mention creates a type local to that
 * prototype — which then refuses to match the real one, and is reported as
 * "incompatible function pointer types" between two spellings that look
 * identical. They are declared here instead, which costs nothing and does not
 * pull the VFS in.
 */
struct file;
struct inode;
struct dentry;
struct super_block;
struct address_space;
struct vfsmount;
struct path;
struct kiocb;
struct iov_iter;
struct seq_file;
struct kstat;
struct iattr;
struct file_operations;
struct vm_area_struct;
struct vm_fault;
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



/* The integer type device-id tables store driver data in (uapi mod_devicetable). */
typedef unsigned long kernel_ulong_t;

#endif
