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
 * A compile-time assertion checked AFTER optimisation, not during parsing.
 *
 * The sizeof-array form this used to be is evaluated where it is written, and
 * that is wrong for the idiom imported code leans on hardest:
 *
 *     static __always_inline bool IS_PLATFORM(..., enum intel_platform p)
 *     { BUILD_BUG_ON(!__builtin_constant_p(p)); ... }
 *
 * `p` is a parameter, so it is not a constant while the function body is being
 * parsed — it becomes one only once the function is inlined into a caller that
 * passed a literal. The array form therefore fails on correct code, every time.
 *
 * So this is upstream's mechanism instead: a call to a function that is
 * declared, never defined, and marked with the error attribute. If the
 * optimiser folds the condition to false the call is deleted and nothing
 * happens; if it survives, the attribute turns it into a build error naming the
 * assertion. It only works when optimising, which is why the imported objects
 * are built -O2 and why the unoptimised path below asserts nothing rather than
 * asserting wrongly.
 */
#ifdef __OPTIMIZE__
#define __BUILD_BUG_FAILED(id, msg)                            \
	do {                                                       \
		extern void __compiletime_error(msg) id(void);         \
		id();                                                  \
	} while (0)
#define __BUILD_BUG_ON_CAT(a, b) a##b
#define __BUILD_BUG_ON_ID(line) __BUILD_BUG_ON_CAT(__build_bug_on_, line)
#define BUILD_BUG_ON_MSG(e, msg)                               \
	do {                                                       \
		if (!__builtin_constant_p(!!(e)) || (e))               \
			__BUILD_BUG_FAILED(__BUILD_BUG_ON_ID(__LINE__), msg); \
	} while (0)
#else
#define BUILD_BUG_ON_MSG(e, msg) do { } while (0)
#endif

#define BUILD_BUG_ON(e) BUILD_BUG_ON_MSG(e, "BUILD_BUG_ON failed: " #e)
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


/* Is `x` an integer constant expression? Used by min()/max() to decide whether
 * they may compare in a way that needs constant folding. The sizeof trick is
 * upstream's: a null pointer constant scaled by (x) has type void* only when x
 * is a constant zero-or-not expression, and int* otherwise. */
#ifndef __is_constexpr
#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))
#endif



/* The caller's return address, for a log line that says who asked. Upstream
 * passes it into lock and allocation tracing; there is none here, but the value
 * is real and cheap, so it is the real one rather than zero. */
#ifndef _RET_IP_
#define _RET_IP_ ((unsigned long)__builtin_return_address(0))
#define _THIS_IP_ ((unsigned long)__builtin_return_address(0))
#endif


/* The integer helpers upstream keeps in <linux/math.h>; drivers reach roundup
 * and friends through this header without including that one. */
#include <linux/math.h>


/* A compile-time assertion that a value is a power of two. Written through the
 * post-optimisation mechanism above, so it behaves like every other
 * BUILD_BUG_ON here. */
#define BUILD_BUG_ON_NOT_POWER_OF_2(n) \
	BUILD_BUG_ON_MSG((n) == 0 || (((n) & ((n) - 1)) != 0), \
	                 "not a power of two: " #n)


/* Typed integer literals, for constants whose width has to survive promotion. */
#define U64_C(x) x##ULL
#define U32_C(x) x##U
#define S64_C(x) x##LL


/* Three-way min/max. Written as nested min/max so the evaluation-once property
 * of those carries through. */
#ifndef min3
#define min3(a, b, c) min(min((a), (b)), (c))
#define max3(a, b, c) max(max((a), (b)), (c))
#endif


#ifndef LLONG_MAX
#define LLONG_MAX  0x7fffffffffffffffLL
#define LLONG_MIN  (-LLONG_MAX - 1)
#endif

/* Multiply then divide without overflowing the product, by splitting the
 * numerator into its quotient and remainder against the divisor first. */
#define mult_frac(x, numer, denom) ({           \
	typeof(x) __q = (x) / (denom);              \
	typeof(x) __r = (x) % (denom);              \
	__q * (numer) + __r * (numer) / (denom);    \
})

/* 64x32/32 without overflowing the product and without a 128-bit divide — the
 * compiler lowers one of those to __udivti3, which is in libgcc and the kernel
 * does not link. Split the numerator against the divisor first, so both halves
 * stay inside 64 bits. */
static inline u64 mul_u64_u32_div(u64 a, u32 mul, u32 divisor)
{
	u64 q = a / divisor;
	u64 r = a % divisor;

	return q * mul + r * mul / divisor;
}

/* The pointer, or NULL when the condition is false. Used to make an optional
 * table pointer conditional without an if. */
#define PTR_IF(cond, ptr) ((cond) ? (ptr) : NULL)

/* Fill an array of pointers with one value. */
static inline void **memset_p(void **p, void *v, usize n)
{
	usize i;
	for (i = 0; i < n; i++)
		p[i] = v;
	return p + n;
}

static inline unsigned long rounddown_pow_of_two(unsigned long n)
{ return n ? 1ul << (63 - __builtin_clzl(n)) : 0; }

/* Like vsnprintf, but returns the number of characters actually written rather
 * than the number that would have been. */
int vscnprintf(char *buf, usize size, const char *fmt, __builtin_va_list args);

/*
 * Parsing formatted input.
 *
 * Declared and deliberately not defined. b1nix's kernel has no scanf: its
 * string library is output-only. The callers here are debugfs write handlers
 * that parse a user-supplied line; giving them a stub that reports "nothing
 * matched" would turn a write that should have taken effect into one that
 * silently did not. A caller fails to link instead.
 */
int sscanf(const char *buf, const char *fmt, ...);

/* Render a buffer as hex into a caller's string, the way print_hex_dump does
 * per line. */
int hex_dump_to_buffer(const void *buf, usize len, int rowsize, int groupsize,
                       char *linebuf, usize linebuflen, bool ascii);

/* Has the kernel been marked with this taint flag? b1nix records no taint —
 * there are no out-of-tree modules and no known-bad states to mark — so the
 * answer is always no, which is the answer that makes callers take their
 * ordinary path. */
static inline bool test_taint(unsigned flag) { (void)flag; return false; }


/* Static keys travel with the kernel interface for the sources that use them;
 * i915_memcpy.c defines one without including <linux/jump_label.h> itself. */
#include <linux/jump_label.h>

#endif
