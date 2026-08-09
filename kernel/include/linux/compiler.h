/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_COMPILER_H
#define LKPI_LINUX_COMPILER_H

/*
 * Annotations imported source carries everywhere.
 *
 * Most are documentation the compiler ignores. The two that are not are
 * `__user`, which marks a pointer the kernel must copy rather than dereference,
 * and READ_ONCE/WRITE_ONCE, which have to stay single accesses — a compiler
 * free to split or re-load them breaks every lockless algorithm built on them.
 */

#define __user
#define __kernel
#define __iomem
#define __force
#define __must_check
#define __maybe_unused  __attribute__((unused))
#define __always_unused __attribute__((unused))
#define __printf(a, b)  __attribute__((format(printf, a, b)))
#define __aligned(x)    __attribute__((aligned(x)))
#define __packed        __attribute__((packed))
#define __always_inline inline __attribute__((always_inline))
#define noinline        __attribute__((noinline))
#define __init
#define __exit
#define __read_mostly
#define __percpu
#define __rcu
#define __randomize_layout
#define __deprecated    __attribute__((deprecated))
#define __malloc        __attribute__((malloc))
#define __pure          __attribute__((pure))
#define __const         __attribute__((const))
#define __noreturn      __attribute__((noreturn))
#define __cold          __attribute__((cold))
#define __used          __attribute__((used))
#define __weak          __attribute__((weak))
#define __section(s)    __attribute__((section(s)))
#define __counted_by(m)
#define __free(f)
#define __cleanup(f)
#define fallthrough     __attribute__((fallthrough))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define barrier() __asm__ __volatile__("" ::: "memory")

#define READ_ONCE(x)      (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, val) (*(volatile __typeof__(x) *)&(x) = (val))

#endif
