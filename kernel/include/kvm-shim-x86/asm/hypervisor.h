/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_HYPERVISOR_H
#define KVM_SHIM_ASM_HYPERVISOR_H
/* Which hypervisor this kernel runs under, in Linux's terms (M131). */
#include <linux/types.h>
enum x86_hypervisor_type {
	X86_HYPER_NATIVE = 0,
	X86_HYPER_VMWARE,
	X86_HYPER_MS_HYPERV,
	X86_HYPER_XEN_PV,
	X86_HYPER_XEN_HVM,
	X86_HYPER_KVM,
	X86_HYPER_JAILHOUSE,
	X86_HYPER_ACRN,
	X86_HYPER_BHYVE,
};
enum x86_hypervisor_type lkpi_x86_hypervisor_type(void);
static inline bool hypervisor_is_type(enum x86_hypervisor_type type)
{
	return lkpi_x86_hypervisor_type() == type;
}
#endif
