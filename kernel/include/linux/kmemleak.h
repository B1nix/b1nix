/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_KMEMLEAK_H
#define LKPI_LINUX_KMEMLEAK_H
#include <linux/types.h>
/* Leak-tracking hints. b1nix's kheap has its own canary checking but no leak
 * scanner these could inform, so they compile away. */
static inline void kmemleak_not_leak(const void *p) { (void)p; }
static inline void kmemleak_ignore(const void *p) { (void)p; }

static inline void kmemleak_update_trace(const void *p) { (void)p; }

/* b1nix has no leak detector to tell that part of an object went away. */
static inline void kmemleak_free_part(const void *p, size_t size) { (void)p; (void)size; }
static inline void kmemleak_free_part_phys(phys_addr_t p, size_t size) { (void)p; (void)size; }

#endif
