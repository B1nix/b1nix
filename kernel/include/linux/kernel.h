/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KERNEL_H
#define LKPI_LINUX_KERNEL_H

#include <linux/cache.h>
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
 * Kconfig tests.
 *
 * b1nix has no Kconfig file; what it has is the -D list in the Makefile, and
 * an option is on exactly when that list defines it as 1. This is upstream's
 * own trick for asking that question in the preprocessor, and it is here
 * because answering 0 to everything is not honest once the build really does
 * turn options on: ext4 refused to mount a filesystem with quotas saying "the
 * kernel was not built with CONFIG_QUOTA" while dquot.c sat in the same image.
 */
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define __is_defined(x) ___is_defined(x)

#define IS_ENABLED(cfg)  __is_defined(cfg)

/* Kconfig values imported code reads directly rather than through IS_ENABLED.
 * The defaults are upstream's own. */
#define CONFIG_DRM_FBDEV_OVERALLOC 100
/* Everything b1nix builds is built in; nothing imported is a module here. */
#define IS_BUILTIN(cfg)  IS_ENABLED(cfg)
#define IS_MODULE(cfg)   0
#define IS_REACHABLE(cfg) IS_ENABLED(cfg)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define min(a, b)                    \
	({                               \
		__typeof__(a) __a = (a);     \
		__typeof__(b) __b = (b);     \
		__a < __b ? __a : __b;       \
	})

/* The smaller of two values, ignoring a zero — "no limit" is spelled 0 in the
 * quota code, and a plain min() would make it the tightest limit there is. */
#define min_not_zero(x, y) ({            \
	typeof(x) __x = (x);                 \
	typeof(y) __y = (y);                 \
	__x == 0 ? __y : (__y == 0 ? __x : min(__x, __y)); })

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
 * compiler lowers one of those to __udivti3, which is in compiler-rt and the kernel
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

/*
 * Is [val, val+len) inside [start, start+size)?
 *
 * Written as one helper because the by-hand form is where an off-by-one lives:
 * the end of a range is start+size, exclusive, and a `<=` there admits one
 * element past it. btrfs checks every on-disk offset it reads through this.
 */
static inline bool in_range64(u64 val, u64 start, u64 len)
{
	return val >= start && val < start + len;
}
#define in_range(val, start, len) in_range64((u64)(val), (u64)(start), (u64)(len))

/*
 * A size with an optional K/M/G/T suffix, as a mount option or a sysfs write
 * spells it. Returns the value and, through `retptr`, where it stopped — a
 * caller checks that to reject trailing junk, which is the difference between
 * accepting "16M" and accepting "16Mb-please".
 */
unsigned long long memparse(const char *ptr, char **retptr);

#ifndef ULLONG_MAX
#define ULLONG_MAX (~0ULL)
#endif
#ifndef ULONG_MAX
#define ULONG_MAX  (~0UL)
#endif

#define high_16_bits(x) (((x) & 0xFFFF0000) >> 16)
#define low_16_bits(x)  ((x) & 0xFFFF)

/* Set some bits and clear others in one atomic step, returning whether the
 * word changed. Two separate operations would let a concurrent reader see the
 * half-updated value — which for an inode's flags is a file that is briefly
 * neither immutable nor mutable. */
/*
 * Type-generic, as upstream's macro is. A function on unsigned long took
 * &inode->i_flags -- an unsigned int -- and compare-exchanged eight bytes over
 * a four-byte field: the neighbour was overwritten on x86_64, and aarch64's
 * exclusive load faulted on the alignment.
 */
#define set_mask_bits(ptr, mask, bits)                                         \
	({                                                                         \
		const typeof(*(ptr)) mask__ = (mask), bits__ = (bits);                 \
		typeof(*(ptr)) old__, new__;                                           \
		do {                                                                   \
			old__ = __atomic_load_n((ptr), __ATOMIC_RELAXED);                  \
			new__ = (old__ & ~mask__) | bits__;                                \
		} while (new__ != old__ &&                                             \
		         !__atomic_compare_exchange_n((ptr), &old__, new__, 0,         \
		                                      __ATOMIC_ACQ_REL,                 \
		                                      __ATOMIC_RELAXED));              \
		new__;                                                                 \
	})

/* Print the current call stack. Real: it is what a filesystem calls when it
 * finds an inconsistency it is going to continue past, and a silent version
 * would throw away the only evidence. */
void dump_stack(void);

/* How the machine is doing overall, so a filesystem can tell an ordinary
 * unmount from one during shutdown and skip work that only matters if the
 * machine keeps running. */
enum system_states {
	SYSTEM_BOOTING,
	SYSTEM_SCHEDULING,
	SYSTEM_FREEING_INITMEM,
	SYSTEM_RUNNING,
	SYSTEM_HALT,
	SYSTEM_POWER_OFF,
	SYSTEM_RESTART,
	SYSTEM_SUSPEND,
};
extern enum system_states system_state;

/* Unaligned loads and stores. Declared here as well as in <asm/unaligned.h>
 * because btrfs reads on-disk fields from files that include neither. */
#include <asm/unaligned.h>

/* Unsigned min/max: both sides widened to u64 so a mixed-sign comparison
 * cannot flip. MIN/MAX are the constant-expression forms. */
#define umin(x, y) min_t(u64, (x), (y))
#define umax(x, y) max_t(u64, (x), (y))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* Constant-expression typed min/max. */
#define MIN_T(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define MAX_T(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))

/* Integer square root, rounded down. */
static inline unsigned long int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;

	if (x <= 1)
		return x;
	m = 1UL << ((63 - __builtin_clzl(x)) & ~1UL);
	while (m != 0) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return y;
}

/* The smallest and largest element of a non-empty array. */
#define __minmax_array(op, array, len) ({                          \
	__typeof__(&(array)[0]) __array = (array);                 \
	__typeof__(len) __len = (len);                             \
	__typeof__(__array[0] + 0) __element = __array[--__len];   \
	while (__len--)                                            \
		__element = op(__element, __array[__len]);         \
	__element; })
#define min_array(array, len) __minmax_array(min, array, len)
#define max_array(array, len) __minmax_array(max, array, len)

/* base^exp in u64, by squaring. */
static inline u64 int_pow(u64 base, unsigned int exp)
{
	u64 result = 1;

	while (exp) {
		if (exp & 1)
			result *= base;
		exp >>= 1;
		base *= base;
	}
	return result;
}

#endif
