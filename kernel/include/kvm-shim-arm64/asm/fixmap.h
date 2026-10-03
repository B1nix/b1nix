/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_FIXMAP_H
#define KVM_SHIM_ARM64_ASM_FIXMAP_H
/*
 * Fixed mappings, as nested KVM uses them: one page per CPU at a fixed
 * address, through which the host maps a nested guest's VNCR page at EL2.
 * The slots count down from the top of a window the arm64 KVM glue reserves;
 * mapping one installs a kernel PTE there and invalidates the old one.
 */
#include <linux/types.h>
#include <linux/threads.h>

enum fixed_addresses {
	FIX_HOLE,
	FIX_VNCR_END,
	FIX_VNCR = FIX_VNCR_END + NR_CPUS,
	__end_of_fixed_addresses
};

unsigned long lkpi_fixaddr_top(void);
#define __fix_to_virt(x)	(lkpi_fixaddr_top() - ((unsigned long)(x) << PAGE_SHIFT))
#define fix_to_virt(x)		__fix_to_virt(x)

#define FIXMAP_PAGE_CLEAR	__pgprot(0)
void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot);
#define clear_fixmap(idx)	__set_fixmap(idx, 0, FIXMAP_PAGE_CLEAR)
#endif
