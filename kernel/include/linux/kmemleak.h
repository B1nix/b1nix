/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_KMEMLEAK_H
#define LKPI_LINUX_KMEMLEAK_H
#include <linux/types.h>
/* Leak-tracking hints. b1nix's kheap has its own canary checking but no leak
 * scanner these could inform, so they compile away. */
static inline void kmemleak_not_leak(const void *p) { (void)p; }
static inline void kmemleak_ignore(const void *p) { (void)p; }

static inline void kmemleak_update_trace(const void *p) { (void)p; }

#endif
