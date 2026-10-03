/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_ALTERNATIVE_H
#define KVM_SHIM_ASM_ALTERNATIVE_H
/* No boot-time instruction patching: an alternative is its original
 * instruction, and the replacement is taken by testing the feature where it
 * matters (the callers that need the new instruction check it themselves). */
#define ALTERNATIVE(oldinstr, newinstr, ft_flags) oldinstr
#define ALTERNATIVE_2(oldinstr, newinstr1, ft1, newinstr2, ft2) oldinstr
#define alternative(oldinstr, newinstr, ft_flags) \
	__asm__ volatile(oldinstr ::: "memory")
#define alternative_io(oldinstr, newinstr, ft_flags, output, input...) \
	__asm__ volatile(oldinstr : output : input)
#define ASM_NO_INPUT_CLOBBER(clbr...) "i"(0) : clbr
#endif
