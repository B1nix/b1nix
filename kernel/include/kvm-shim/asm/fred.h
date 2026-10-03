/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_FRED_H
#define KVM_SHIM_ASM_FRED_H
#include <linux/types.h>
/* b1nix does not run with FRED: the IDT path is the one KVM takes. */
static inline void fred_entry_from_kvm(unsigned int type, unsigned int vector)
{
	(void)type; (void)vector;
}
#endif
