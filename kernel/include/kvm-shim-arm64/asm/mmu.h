/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_MMU_H
#define KVM_SHIM_ARM64_ASM_MMU_H
#include <linux/const.h>
#include <linux/types.h>
/* The TTBR ASID layout KVM's TLB and stage-1 code names. b1nix does not unmap
 * the kernel at EL0 (no KPTI), so no trampoline mapping exists to switch. */
#define USER_ASID_BIT	48
#define USER_ASID_FLAG	(UL(1) << USER_ASID_BIT)
#define TTBR_ASID_MASK	(UL(0xffff) << 48)
#ifndef __ASSEMBLY__
static inline bool arm64_kernel_unmapped_at_el0(void) { return false; }
#endif
/* b1nix runs every address space on ASID 0 and invalidates by VA across all
 * ASIDs (kernel/arch/aarch64/paging.c), so an mm's ASID is always 0. */
#define ASID(mm)	((void)(mm), 0UL)
#include <asm/pgtable-prot.h>
#endif
