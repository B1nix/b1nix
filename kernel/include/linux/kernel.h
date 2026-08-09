/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_KERNEL_H
#define LKPI_LINUX_KERNEL_H

#include <b1nix/types.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/container_of.h>
#include <linux/bits.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/lockdep.h>

/*
 * The grab-bag imported source includes for min/max, rounding and
 * container_of. Each is written to evaluate its arguments once, because
 * callers pass expressions with side effects and the double-evaluation bug
 * that follows is silent.
 */

/*
 * Kconfig tests. b1nix has no Kconfig, so every option imported source asks
 * about is off. That is a statement about the build, not a stub: an option
 * reported on that is not implemented would be far worse than one reported off.
 */
#define IS_ENABLED(cfg)  0

/* Kconfig values imported code reads directly rather than through IS_ENABLED.
 * The defaults are upstream's own. */
#define CONFIG_DRM_FBDEV_OVERALLOC 100
#define IS_BUILTIN(cfg)  0
#define IS_MODULE(cfg)   0
#define IS_REACHABLE(cfg) 0

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define min(a, b)                    \
	({                               \
		__typeof__(a) __a = (a);     \
		__typeof__(b) __b = (b);     \
		__a < __b ? __a : __b;       \
	})

#define max(a, b)                    \
	({                               \
		__typeof__(a) __a = (a);     \
		__typeof__(b) __b = (b);     \
		__a > __b ? __a : __b;       \
	})

#define min_t(type, a, b)            \
	({                               \
		type __a = (type)(a);        \
		type __b = (type)(b);        \
		__a < __b ? __a : __b;       \
	})

#define max_t(type, a, b)            \
	({                               \
		type __a = (type)(a);        \
		type __b = (type)(b);        \
		__a > __b ? __a : __b;       \
	})

#define clamp(v, lo, hi)  min(max(v, lo), hi)
#define clamp_t(type, v, lo, hi) min_t(type, max_t(type, v, lo), hi)
#define clamp_val(v, lo, hi) clamp_t(__typeof__(v), v, lo, hi)

#define abs(x)                       \
	({                               \
		__typeof__(x) __x = (x);     \
		__x < 0 ? -__x : __x;        \
	})

#define swap(a, b)                   \
	do {                             \
		__typeof__(a) __t = (a);     \
		(a) = (b);                   \
		(b) = __t;                   \
	} while (0)

#define round_up(x, y)   (((x) + ((y) - 1)) & ~((__typeof__(x))(y) - 1))
#define round_down(x, y) ((x) & ~((__typeof__(x))(y) - 1))
#define DIV_ROUND_UP(n, d)      (((n) + (d) - 1) / (d))
#define DIV_ROUND_DOWN_ULL(n, d) ((n) / (d))
#define DIV_ROUND_CLOSEST(n, d) (((n) + (d) / 2) / (d))
#define ALIGN(x, a)             (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a)        ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a)        (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a)         ((__typeof__(p))ALIGN((usize)(p), (a)))

#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int : (-!!(e)); }))
/*
 * Only assert when the expression really is a constant. Imported code writes
 * BUILD_BUG_ON over expressions the compiler folds at -O2 but not at -O0, and
 * failing to fold is not the same as failing the assertion — it would turn an
 * optimisation difference into a build error.
 */
#define BUILD_BUG_ON(e)                                        \
	((void)sizeof(char[__builtin_constant_p(e) ? 1 - 2 * !!(e) : 1]))
#define BUILD_BUG_ON_INVALID(e) ((void)(0 && (e)))
#define BUILD_BUG()          BUILD_BUG_ON(1)

#define TASK_COMM_LEN 16

#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif

/* A userspace pointer that travelled through an ioctl struct as a 64-bit
 * integer. The cast is where it stops being a number, so it is spelled out. */
/* Pixel clock in kHz to a period in picoseconds, and back. The rounding is
 * upstream's; a different one shifts reported refresh rates. */
#define KHZ2PICOS(a) (1000000000UL / (a))

#define u64_to_user_ptr(x) ((void __user *)(usize)(x))

#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n) ((u32)((n) & 0xffffffffu))

#endif
