/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_STACKTRACE_H
#define LKPI_LINUX_STACKTRACE_H
#include <linux/types.h>
/* b1nix has a backtrace printer (panic path) but no unwinder that fills a
 * caller's array, so these report "no frames" rather than invent any. */
static inline unsigned int stack_trace_save(unsigned long *store,
                                            unsigned int size,
                                            unsigned int skipnr)
{ (void)store; (void)size; (void)skipnr; return 0; }
#endif
