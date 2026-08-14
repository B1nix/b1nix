/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_TIMEKEEPING_H
#define LKPI_LINUX_TIMEKEEPING_H
#include <linux/ktime.h>
#include <linux/types.h>
#include <lkpi/env.h>

/* ktime_get_raw_ns() lives in <linux/ktime.h>, included above — the same
 * tick-derived clock, declared next to the type it returns. */
static inline s64 ktime_to_ns_safe(ktime_t t) { return ktime_to_ns(t); }

/* The NMI-safe reader ktime_get_raw_fast_ns() lives in <linux/ktime.h> — same
 * tick counter, reached through the header that already declares ktime_t. */

#endif
