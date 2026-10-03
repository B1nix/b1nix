/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_TEXT_PATCHING_H
#define KVM_SHIM_ASM_TEXT_PATCHING_H
/* The emulator only needs the instruction lengths text poking uses. */
#define CALL_INSN_SIZE 5
#define JMP32_INSN_SIZE 5
#define RET_INSN_SIZE 1
#define INT3_INSN_SIZE 1
#include <linux/types.h>
#include <asm/processor-flags.h>
/* Does condition code cc hold for these flags (upstream's helper). */
static inline bool __emulate_cc(unsigned long flags, u8 cc)
{
	static const unsigned long cc_mask[6] = {
		[0] = X86_EFLAGS_OF,
		[1] = X86_EFLAGS_CF,
		[2] = X86_EFLAGS_ZF,
		[3] = X86_EFLAGS_CF | X86_EFLAGS_ZF,
		[4] = X86_EFLAGS_SF,
		[5] = X86_EFLAGS_PF,
	};
	bool invert = cc & 1;
	bool match;

	if (cc < 0xc) {
		match = flags & cc_mask[cc >> 1];
	} else {
		match = ((flags & X86_EFLAGS_SF) >> X86_EFLAGS_SF_BIT) ^
			((flags & X86_EFLAGS_OF) >> X86_EFLAGS_OF_BIT);
		if (cc >= 0xe)
			match = match || (flags & X86_EFLAGS_ZF);
	}
	return (match && !invert) || (!match && invert);
}
#endif
