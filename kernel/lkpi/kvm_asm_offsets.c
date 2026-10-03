// SPDX-License-Identifier: GPL-2.0-only
/*
 * The structure offsets KVM's entry code (vmx/vmenter.S, svm/vmenter.S) reads
 * (M131): upstream's arch/x86/kvm/kvm-asm-offsets.c, the same list. It is
 * compiled with -S and the Makefile turns its markers into
 * build/.../kvm-asm-offsets.h, exactly as Kbuild does.
 */
#include <linux/kbuild.h>
#include "vmx/vmx.h"
#include "svm/svm.h"

void kvm_asm_offsets(void);
void kvm_asm_offsets(void)
{
	OFFSET(SVM_vcpu_arch_regs, vcpu_svm, vcpu.arch.regs);
	OFFSET(SVM_current_vmcb, vcpu_svm, current_vmcb);
	OFFSET(SVM_spec_ctrl, vcpu_svm, spec_ctrl);
	OFFSET(SVM_vmcb01, vcpu_svm, vmcb01);
	OFFSET(KVM_VMCB_pa, kvm_vmcb_info, pa);
	OFFSET(SD_save_area_pa, svm_cpu_data, save_area_pa);
	OFFSET(VMX_spec_ctrl, vcpu_vmx, spec_ctrl);
}
