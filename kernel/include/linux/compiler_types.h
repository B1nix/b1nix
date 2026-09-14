/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_COMPILER_TYPES_H
#define LKPI_LINUX_COMPILER_TYPES_H
#include <linux/compiler.h>
/*
 * static_assert, here rather than in <linux/build_bug.h> where it belongs by
 * subject.
 *
 * The reason is the include graph. Imported headers use it at file scope
 * without including anything that would define it, and they get away with it
 * upstream because the kernel force-includes a header that reaches it. Routing
 * it through <linux/build_bug.h> here means <linux/kernel.h>, which means
 * <linux/bitops.h> and <linux/bitmap.h> — and those need <linux/errno.h>, which
 * <linux/types.h> has not reached yet at that point. The cycle showed up as
 * `use of undeclared identifier ENOMEM` inside bitmap.h.
 *
 * This file is force-included by every imported translation unit and depends on
 * nothing, which makes it the one place the definition can go.
 *
 * The one-argument form is upstream's extension: C11 requires a message, so the
 * expression is stringified into one.
 */
#ifndef static_assert
#define static_assert(expr, ...) _Static_assert(expr, #expr)
#endif

/* A name no other use of the same prefix in this translation unit can collide
 * with; the scoped guards in <linux/cleanup.h> declare their variables with it. */
#ifndef __UNIQUE_ID
#define ___PASTE(a, b) a##b
#define __PASTE(a, b) ___PASTE(a, b)
#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)
#endif

/* Scoped diagnostic suppression. The imported code is built with -w, so there
 * is nothing to suppress; each form is a declaration that consumes the `;`
 * that follows it at file or block scope. */
#ifndef __diag_push
#define __diag_push()                   _Static_assert(1, "")
#define __diag_pop()                    _Static_assert(1, "")
#define __diag_ignore_all(option, comment) _Static_assert(1, "")
#endif

#endif
