/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_AGP_BACKEND_H
#define LKPI_LINUX_AGP_BACKEND_H
#include <linux/types.h>
/* AGP. The bus is long dead and no target of M102 uses it; the types exist so
 * the core's legacy paths compile, and every entry point reports absence. */
struct agp_bridge_data;
struct agp_kern_info { unsigned long aper_base; usize aper_size; };
struct agp_memory { usize page_count; };
static inline struct agp_bridge_data *agp_backend_acquire(void *dev)
{ (void)dev; return 0; }
static inline void agp_backend_release(struct agp_bridge_data *b) { (void)b; }

/* The memory types the GMCH aperture is programmed with. Upstream's numbers,
 * because they are written into a chipset register: i915's Gen2-Gen5 path
 * passes one of these to intel_gmch_gtt_insert_*, and it is only the type tag
 * for the entry it would write. */
#define AGP_USER_TYPES         (1 << 16)
#define AGP_USER_MEMORY        (AGP_USER_TYPES)
#define AGP_USER_CACHED_MEMORY (AGP_USER_TYPES + 1)
#endif
