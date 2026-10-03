// SPDX-License-Identifier: GPL-2.0-only
/*
 * The structure offsets arm64 KVM's world-switch code (hyp/entry.S,
 * hyp/hyp-entry.S) reads (M131): the CONFIG_KVM part of upstream's
 * arch/arm64/kernel/asm-offsets.c. Compiled with -S; the Makefile turns its
 * markers into build/.../kvm-gen/asm/asm-offsets.h, as Kbuild does.
 */
#include <linux/kbuild.h>
#include <linux/kvm_host.h>

void kvm_arm64_asm_offsets(void);
void kvm_arm64_asm_offsets(void)
{
	DEFINE(VCPU_CONTEXT,		offsetof(struct kvm_vcpu, arch.ctxt));
	DEFINE(VCPU_FAULT_DISR,		offsetof(struct kvm_vcpu, arch.fault.disr_el1));
	DEFINE(VCPU_HCR_EL2,		offsetof(struct kvm_vcpu, arch.hcr_el2));
	DEFINE(CPU_USER_PT_REGS,	offsetof(struct kvm_cpu_context, regs));
	DEFINE(CPU_ELR_EL2,		offsetof(struct kvm_cpu_context, sys_regs[ELR_EL2]));
	DEFINE(CPU_RGSR_EL1,		offsetof(struct kvm_cpu_context, sys_regs[RGSR_EL1]));
	DEFINE(CPU_GCR_EL1,		offsetof(struct kvm_cpu_context, sys_regs[GCR_EL1]));
	DEFINE(CPU_APIAKEYLO_EL1,	offsetof(struct kvm_cpu_context, sys_regs[APIAKEYLO_EL1]));
	DEFINE(CPU_APIBKEYLO_EL1,	offsetof(struct kvm_cpu_context, sys_regs[APIBKEYLO_EL1]));
	DEFINE(CPU_APDAKEYLO_EL1,	offsetof(struct kvm_cpu_context, sys_regs[APDAKEYLO_EL1]));
	DEFINE(CPU_APDBKEYLO_EL1,	offsetof(struct kvm_cpu_context, sys_regs[APDBKEYLO_EL1]));
	DEFINE(CPU_APGAKEYLO_EL1,	offsetof(struct kvm_cpu_context, sys_regs[APGAKEYLO_EL1]));
	DEFINE(HOST_CONTEXT_VCPU,	offsetof(struct kvm_cpu_context, __hyp_running_vcpu));
	DEFINE(HOST_DATA_CONTEXT,	offsetof(struct kvm_host_data, host_ctxt));
}
