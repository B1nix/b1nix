/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_PAGE_TYPES_H
#define KVM_SHIM_ASM_PAGE_TYPES_H
#include <linux/const.h>
#include <asm/page.h>
#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE (_AC(1, UL) << PAGE_SHIFT)
#endif
#ifndef PAGE_MASK
#define PAGE_MASK (~(PAGE_SIZE - 1))
#endif
#define PMD_SHIFT 21
#define PUD_SHIFT 30
#define PGDIR_SHIFT 39
#define __PHYSICAL_MASK_SHIFT 52
#define __VIRTUAL_MASK_SHIFT 47
#define __PHYSICAL_MASK ((phys_addr_t)((1ULL << __PHYSICAL_MASK_SHIFT) - 1))
#define PHYSICAL_PAGE_MASK (((signed long)PAGE_MASK) & __PHYSICAL_MASK)
#endif
