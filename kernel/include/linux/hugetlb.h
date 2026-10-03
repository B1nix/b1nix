/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_HUGETLB_H
#define LKPI_LINUX_HUGETLB_H
#include <linux/mm.h>
/* No hugetlbfs: a VMA is never a hugetlb one. (Transparent huge pages are a
 * different thing, seen through the page tables.) */
static inline bool is_vm_hugetlb_page(struct vm_area_struct *vma) { (void)vma; return false; }

/* b1nix has no hugetlbfs, so no folio is a hugetlb folio and no VMA has a
 * huge-page state; code asks is_vm_hugetlb_page() first, and a call past it
 * is a bug. */
struct folio;
struct hstate;
static inline bool folio_test_hugetlb(const struct folio *folio) { (void)folio; return false; }
static inline struct hstate *hstate_vma(struct vm_area_struct *vma)
{ (void)vma; BUG(); return NULL; }
static inline unsigned int huge_page_shift(struct hstate *h) { (void)h; BUG(); return 0; }

#endif
