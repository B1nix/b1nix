/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_STACKTRACE_H
#define LKPI_LINUX_STACKTRACE_H
#include <linux/types.h>
/* b1nix has a backtrace printer (panic path) but no unwinder that fills a
 * caller's array, so these report "no frames" rather than invent any. */
static inline unsigned int stack_trace_save(unsigned long *store,
                                            unsigned int size,
                                            unsigned int skipnr)
{ (void)store; (void)size; (void)skipnr; return 0; }
/* A stack walker's per-frame callback: false stops the walk. */
typedef bool (*stack_trace_consume_fn)(void *cookie, unsigned long addr);

#endif
