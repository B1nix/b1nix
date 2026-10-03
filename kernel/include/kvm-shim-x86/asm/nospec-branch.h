/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_NOSPEC_BRANCH_H
#define KVM_SHIM_ASM_NOSPEC_BRANCH_H
#ifdef __ASSEMBLY__
#include <asm/msr-index.h>
#include <asm/percpu.h>
/* The return-stack and buffer-clearing mitigations are not applied (b1nix
 * applies none of its own), and ALTERNATIVE keeps its original instruction,
 * as on the C side (asm/alternative.h). */
.macro CLEAR_CPU_BUFFERS
.endm
.macro CLEAR_BRANCH_HISTORY_VMEXIT
.endm
.macro FILL_RETURN_BUFFER reg:req nr:req ftr:req ftr2
.endm
.macro UNTRAIN_RET
.endm
.macro CALL_NOSPEC reg:req
	call *%\reg
.endm
.macro ALTERNATIVE oldinstr, newinstr, ft_flags
	\oldinstr
.endm
.macro ALTERNATIVE_2 oldinstr, newinstr1, ft_flags1, newinstr2, ft_flags2
	\oldinstr
.endm
.macro VM_CLEAR_CPU_BUFFERS
.endm
.macro UNTRAIN_RET_VM
.endm
#else
/* Speculation barriers around guest entry (M131). IBPB is issued when the
 * CPU has it; the per-CPU SPEC_CTRL value is b1nix's (it does not change
 * SPEC_CTRL, so the host value is whatever the firmware left). The mitigation
 * switches (cpu_buf_vm_clear, switch_vcpu_ibpb) are off: b1nix applies no
 * MDS/L1TF mitigations of its own, and turning them on for guests only would
 * be half a policy. */
#include <linux/types.h>
#include <linux/jump_label.h>
#include <linux/percpu.h>
#include <asm/msr.h>
#include <asm/cpufeature.h>

static inline void indirect_branch_prediction_barrier(void)
{
	if (boot_cpu_has(X86_FEATURE_IBPB))
		native_wrmsrq(MSR_IA32_PRED_CMD, PRED_CMD_IBPB);
}
DECLARE_PER_CPU(u64, x86_spec_ctrl_current);
DECLARE_PER_CPU(bool, x86_ibpb_exit_to_user);
extern u64 x86_pred_cmd;
DECLARE_STATIC_KEY_FALSE(switch_vcpu_ibpb);
DECLARE_STATIC_KEY_FALSE(cpu_buf_vm_clear);
DECLARE_STATIC_KEY_FALSE(cpu_buf_idle_clear);
#endif
#endif
