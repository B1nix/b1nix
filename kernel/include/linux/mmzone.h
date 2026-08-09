/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_MMZONE_H
#define LKPI_LINUX_MMZONE_H
#include <linux/mm.h>
/* Memory zones and NUMA nodes. b1nix has one flat zone and one node, so the
 * allocator has nothing to choose between and these constants say so. */
#define MAX_ORDER 11
#define NUMA_NO_NODE (-1)
static inline int page_to_nid(const struct page *p) { (void)p; return 0; }
#endif
