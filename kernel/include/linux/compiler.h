/* SPDX-License-Identifier: GPL-2.0-only */
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


/* One iteration of a spin loop. Not just a pause instruction here: b1nix
 * services TLB shootdowns from this path, so a CPU spinning without it can
 * leave the CPU that sent one waiting forever. Declared rather than inlined
 * because the servicing lives on b1nix's side of the boundary. */
void lkpi_cpu_relax(void);
/* See b1nix/arch.h: one sentinel, whichever header lands first. */
#ifndef B1NIX_CPU_RELAX_DEFINED
#define B1NIX_CPU_RELAX_DEFINED 1
static inline void cpu_relax(void) { lkpi_cpu_relax(); }
#endif


/*
 * Sparse's lock annotations.
 *
 * Upstream writes `__acquires(lock)` on a function that returns holding a lock
 * and `__releases(lock)` on one that returns having dropped it, so a static
 * checker can find the paths that do neither. Nothing here runs sparse, so they
 * carry no meaning — but they sit between the declarator and the body, so
 * leaving them undefined is a syntax error rather than a missing check.
 */
#ifndef __acquires
#define __acquires(x)
#define __releases(x)
#define __acquire(x)   (void)0
#define __release(x)   (void)0
#define __must_hold(x)
#define __cond_lock(x, c) (c)
#endif


/* The attribute that makes a surviving call a build error, naming the failed
 * assertion. Clang and GCC both spell it this way. */
#ifndef __compiletime_error
#define __compiletime_error(msg) __attribute__((__error__(msg)))
#endif


/* Do two expressions have the same type? Used by the container_of family and by
 * the array-size guard, where the point is to reject a pointer that decayed
 * from an array. */
#ifndef __same_type
#define __same_type(a, b) __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
#endif

#endif
