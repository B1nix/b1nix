/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_VMALLOC_H
#define LKPI_LINUX_VMALLOC_H
#include <linux/slab.h>
#include <lkpi/page.h>
/* Linux's vmalloc gives a linear range backed by scattered pages. b1nix's
 * kheap already hands back kernel-linear memory, so for a plain allocation
 * these are kmalloc; the scattered-pages case that actually needs stitching is
 * lkpi_vmap, and callers that mean that use it by name. */
static inline void *vmalloc(usize size) { return lkpi_kmalloc(size, GFP_KERNEL); }
static inline void *vzalloc(usize size)
{ return lkpi_kmalloc(size, GFP_KERNEL | __GFP_ZERO); }
static inline void vfree(const void *p) { lkpi_kfree((void *)p); }
#endif
