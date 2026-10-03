/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_ASSEMBLER_H
#define KVM_SHIM_ARM64_ASM_ASSEMBLER_H
/*
 * Upstream's assembler macros, with one difference. KVM's world-switch code
 * reaches a per-CPU variable as its symbol plus the CPU's per-CPU offset,
 * which Linux keeps in TPIDR_EL2 when it runs at EL2. b1nix's TPIDR_EL2
 * points at its own per-CPU block instead (kernel/arch/aarch64/smp.c), and the
 * offset of KVM's per-CPU copy is a word in that block, at +32.
 */
#include_next <asm/assembler.h>

#ifdef __ASSEMBLY__
	.purgem	get_this_cpu_offset
	.macro	get_this_cpu_offset, dst
	mrs	\dst, tpidr_el2
	ldr	\dst, [\dst, #32]
	.endm
#endif
#endif
