/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_HUGETLB_H
#define LKPI_LINUX_HUGETLB_H
#include <linux/mm.h>
/* No hugetlbfs: a VMA is never a hugetlb one. (Transparent huge pages are a
 * different thing, seen through the page tables.) */
static inline bool is_vm_hugetlb_page(struct vm_area_struct *vma) { (void)vma; return false; }
#endif
