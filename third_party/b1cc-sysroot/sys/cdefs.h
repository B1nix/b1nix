#ifndef _SYS_CDEFS_H
#define _SYS_CDEFS_H

/* Minimal <sys/cdefs.h> for the b1nix libc (added for the Chromium port,
 * M60-62). This BSD/glibc compatibility header only defines feature macros that
 * portable C/C++ headers expect; it has no runtime component. Values mirror the
 * common glibc/BSD definitions so third-party code (partition_alloc's
 * allocator_shim, etc.) compiles unchanged. */

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

/* glibc expands __THROW/__THROWNL to a C++ exception specification (empty in C).
 * Use noexcept for C++11 and newer, but keep C++98-compatible throw() for
 * runtimes such as libstdc++ that still build some files in older language
 * modes. */
#ifndef __THROW
#ifdef __cplusplus
#if __cplusplus >= 201103L
#define __THROW noexcept(true)
#define __THROWNL noexcept(true)
#else
#define __THROW throw()
#define __THROWNL throw()
#endif
#else
#define __THROW
#define __THROWNL
#endif
#endif
#ifndef __THROWNL
#define __THROWNL
#endif
#ifndef __nonnull
#define __nonnull(params)
#endif
#ifndef __wur
#define __wur
#endif
#ifndef __attribute_malloc__
#define __attribute_malloc__
#endif
#ifndef __attribute_pure__
#define __attribute_pure__
#endif
#ifndef __attribute_const__
#define __attribute_const__
#endif
#ifndef __attribute_used__
#define __attribute_used__
#endif
#ifndef __attribute_noinline__
#define __attribute_noinline__
#endif
#ifndef __flexarr
#define __flexarr []
#endif

#ifndef __BEGIN_NAMESPACE_STD
#define __BEGIN_NAMESPACE_STD
#define __END_NAMESPACE_STD
#define __USING_NAMESPACE_STD(name)
#endif

/* __restrict is a GCC/clang keyword in both C and C++. Do NOT remap it to the
 * C99 `restrict` keyword in C++ (where `restrict` is not a keyword) — that breaks
 * C++ code like Abseil's `void* __restrict dst`. Only alias it in C, and only if
 * the compiler somehow lacks it. */
#if !defined(__cplusplus) && !defined(__GNUC__) && !defined(__clang__)
#ifndef __restrict
#define __restrict restrict
#endif
#endif

#ifndef __P
#define __P(args) args
#endif

#endif /* _SYS_CDEFS_H */
