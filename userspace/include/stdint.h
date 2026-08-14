#ifndef B1NIX_U_STDINT_H
#define B1NIX_U_STDINT_H

/* x86_64 is LP64, so the 64-bit exact-width types are `long`, the same choice
 * musl makes (bits/alltypes.h: #define _Int64 long) and the same one the rest
 * of the LP64 world makes. This header said `long long` for years. It is
 * normally shadowed — every build puts musl's headers ahead of this directory —
 * but where it is not, the mismatch is invisible until something reasons about
 * the spelling rather than the width: Skia's dng_sdk picks
 * __builtin_smull_overflow off `LONG_MAX == INT64_MAX` and takes a `long *`,
 * which does not accept `long long *`, and that disagreement is why a b1nix
 * carve-out was patched into dng_safe_arithmetic.h. */
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long int64_t;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

typedef long intptr_t;
typedef unsigned long uintptr_t;

typedef long intmax_t;
typedef unsigned long uintmax_t;

/* least/fast-width types (exact-width for least; int-or-wider for fast). */
typedef int8_t   int_least8_t;
typedef int16_t  int_least16_t;
typedef int32_t  int_least32_t;
typedef int64_t  int_least64_t;
typedef uint8_t  uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;

typedef int8_t   int_fast8_t;
typedef int      int_fast16_t;
typedef int      int_fast32_t;
typedef int64_t  int_fast64_t;
typedef uint8_t  uint_fast8_t;
typedef unsigned int  uint_fast16_t;
typedef unsigned int  uint_fast32_t;
typedef uint64_t uint_fast64_t;

#define INT8_C(c)  c
#define UINT8_C(c) c
#define INT16_C(c)  c
#define UINT16_C(c) c
#define INT32_C(c)  c
#define UINT32_C(c) c
#define INT64_C(c)  c ## L
#define UINT64_C(c) c ## UL

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255

#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535

#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295U

#define INT64_MIN  (-9223372036854775807L - 1)
#define INT64_MAX  9223372036854775807L
#define UINT64_MAX 18446744073709551615UL

#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#define INTPTR_MIN  (-9223372036854775807L - 1)
#define INTPTR_MAX  9223372036854775807L
#define UINTPTR_MAX 18446744073709551615UL

#define SIZE_MAX  18446744073709551615UL
#define SSIZE_MAX 9223372036854775807L

/* C99 least/fast-width limits. The least types are the exact-width types; the
 * fast 16/32 types are `int`/`unsigned int` (32-bit) on this ABI. Required by
 * libstdc++'s C99-stdint-TR1 probe (gates <random>) and by general C99 code. */
#define INT_LEAST8_MIN   INT8_MIN
#define INT_LEAST8_MAX   INT8_MAX
#define UINT_LEAST8_MAX  UINT8_MAX
#define INT_LEAST16_MIN  INT16_MIN
#define INT_LEAST16_MAX  INT16_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define INT_LEAST32_MIN  INT32_MIN
#define INT_LEAST32_MAX  INT32_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define INT_LEAST64_MIN  INT64_MIN
#define INT_LEAST64_MAX  INT64_MAX
#define UINT_LEAST64_MAX UINT64_MAX

#define INT_FAST8_MIN    INT8_MIN
#define INT_FAST8_MAX    INT8_MAX
#define UINT_FAST8_MAX   UINT8_MAX
#define INT_FAST16_MIN   INT32_MIN
#define INT_FAST16_MAX   INT32_MAX
#define UINT_FAST16_MAX  UINT32_MAX
#define INT_FAST32_MIN   INT32_MIN
#define INT_FAST32_MAX   INT32_MAX
#define UINT_FAST32_MAX  UINT32_MAX
#define INT_FAST64_MIN   INT64_MIN
#define INT_FAST64_MAX   INT64_MAX
#define UINT_FAST64_MAX  UINT64_MAX

#define PTRDIFF_MIN      INTPTR_MIN
#define PTRDIFF_MAX      INTPTR_MAX
#define SIG_ATOMIC_MIN   INT32_MIN
#define SIG_ATOMIC_MAX   INT32_MAX
#ifndef WCHAR_MIN
#define WCHAR_MIN        INT32_MIN
#define WCHAR_MAX        INT32_MAX
#endif
#define WINT_MIN         0U
#define WINT_MAX         UINT32_MAX
#define PTRDIFF_MAX 9223372036854775807L
#define PTRDIFF_MIN (-9223372036854775807L - 1)
#define SIG_ATOMIC_MAX 2147483647
#define SIG_ATOMIC_MIN (-2147483647 - 1)
#define WCHAR_MAX 2147483647
#define WCHAR_MIN (-2147483647 - 1)
#define WINT_MAX 4294967295U
#define WINT_MIN 0U

#endif
