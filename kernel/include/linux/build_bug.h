/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_BUILD_BUG_H
#define LKPI_LINUX_BUILD_BUG_H
/* Upstream splits the compile-time assertions out of <linux/kernel.h>; here they
 * were written there first, so this is the split-out name pointing back. */
#include <linux/kernel.h>

#ifndef static_assert
#define static_assert(expr, ...) _Static_assert(expr, #expr)
#endif
#endif
