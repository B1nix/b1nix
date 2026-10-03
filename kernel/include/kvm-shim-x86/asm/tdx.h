/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_TDX_H
#define KVM_SHIM_ASM_TDX_H
#include <linux/errno.h>
/* Neither a TDX host nor a TDX guest. kvm_para.h calls the guest hypercall
 * only behind X86_FEATURE_TDX_GUEST, which is never set. */
static inline long tdx_kvm_hypercall(unsigned int nr, unsigned long p1,
				     unsigned long p2, unsigned long p3,
				     unsigned long p4)
{
	(void)nr; (void)p1; (void)p2; (void)p3; (void)p4;
	return -ENODEV;
}
#endif
