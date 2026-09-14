/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_MMZONE_H
#define LKPI_LINUX_MMZONE_H
#include <linux/mm.h>
/* Memory zones and NUMA nodes. b1nix has one flat zone and one node, so the
 * allocator has nothing to choose between and these constants say so. */
#define MAX_ORDER 11
#define NUMA_NO_NODE (-1)
static inline int page_to_nid(const struct page *p) { (void)p; return 0; }

/* 6.8's names: the largest allocation order, inclusive, and the count. */
#define MAX_PAGE_ORDER (MAX_ORDER - 1)
#define NR_PAGE_ORDERS MAX_ORDER

#endif
