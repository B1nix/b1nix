/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_EXCEPTION_H
#define KVM_SHIM_ARM64_ASM_EXCEPTION_H
/* Linux's exception-entry declarations are b1nix's own; KVM needs only the
 * DISR_EL1 to ESR conversion for an SError a guest exit deferred. */
#include <linux/types.h>
#include <asm/esr.h>
#include <asm/sysreg.h>

static inline unsigned long disr_to_esr(u64 disr)
{
	unsigned long esr = ESR_ELx_EC_SERROR << ESR_ELx_EC_SHIFT;

	if ((disr & DISR_EL1_IDS) == 0)
		esr |= (disr & DISR_EL1_ESR_MASK);
	else
		esr |= (disr & ESR_ELx_ISS_MASK);

	return esr;
}
#endif
