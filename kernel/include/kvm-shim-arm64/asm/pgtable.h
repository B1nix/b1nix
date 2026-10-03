/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_PGTABLE_H
#define KVM_SHIM_ARM64_ASM_PGTABLE_H
/*
 * The arm64 page-table vocabulary KVM uses outside its own stage-2 code
 * (M131). The descriptor layout and protections are upstream's
 * <asm/pgtable-hwdef.h> and <asm/pgtable-prot.h>, imported unchanged, as is
 * the TLB maintenance in <asm/tlbflush.h>. b1nix's physical addresses fit 48
 * bits, so a table's address goes into a TTBR as it is.
 */
#include <linux/types.h>
#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>
#include <asm/pgtable-prot.h>
#include <asm/tlbflush.h>
#include <asm/fixmap.h>

#define phys_to_ttbr(addr)	(addr)
#endif
