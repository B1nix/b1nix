/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_RMAP_H
#define LKPI_LINUX_RMAP_H
#include <linux/mm.h>

/*
 * Write-protect every user mapping of a page-cache folio and report whether
 * any of them was dirty. The imported filesystems' page cache is never mapped
 * into user space here — b1nix's mmap of a file on them copies into its own
 * pages — so there is no mapping to clean and the true answer is "none dirty".
 */
static inline int folio_mkclean(struct folio *folio)
{ (void)folio; return 0; }

#endif
