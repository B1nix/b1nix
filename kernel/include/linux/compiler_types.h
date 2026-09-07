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

#endif
