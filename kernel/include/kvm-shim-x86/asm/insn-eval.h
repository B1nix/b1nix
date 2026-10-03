/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_INSN_EVAL_H
#define KVM_SHIM_ASM_INSN_EVAL_H
#include <linux/types.h>

/* Assign to a register the way the operand size says (a 32-bit write
 * zero-extends), as upstream's helper. */
static inline void insn_assign_reg(unsigned long *reg, u64 val, int bytes)
{
	switch (bytes) {
	case 1: *(u8 *)reg = (u8)val; break;
	case 2: *(u16 *)reg = (u16)val; break;
	case 4: *reg = (u32)val; break;
	case 8: *reg = val; break;
	}
}

#endif
