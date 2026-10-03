/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The nVHE hypervisor's code and per-CPU symbols (M131; see
 * kvm_arm64_nvhe.c). There is no nVHE image: the vectors and the init entry
 * are a BRK, which the host reaches only by a bug, and the per-CPU template
 * is empty -- its start and end are one address.
 */
	.text
	.balign	2048
	.globl	__kvm_nvhe___kvm_hyp_vector
	.globl	__kvm_nvhe___bp_harden_hyp_vecs
	.globl	__kvm_nvhe___kvm_hyp_init
	.globl	__hyp_set_vectors
	.globl	__hyp_reset_vectors
__kvm_nvhe___kvm_hyp_vector:
__kvm_nvhe___bp_harden_hyp_vecs:
__kvm_nvhe___kvm_hyp_init:
__hyp_set_vectors:
__hyp_reset_vectors:
	brk	#0x800

	.data
	.balign	8
	.globl	__kvm_nvhe___per_cpu_start
	.globl	__kvm_nvhe___per_cpu_end
	.globl	__kvm_nvhe_kvm_host_data
	.globl	__kvm_nvhe_kvm_hyp_vector
	.globl	__kvm_nvhe_kvm_init_params
	.globl	__kvm_nvhe_kvm_stacktrace_info
	.globl	__kvm_nvhe_overflow_stack
__kvm_nvhe___per_cpu_start:
__kvm_nvhe_kvm_host_data:
__kvm_nvhe_kvm_hyp_vector:
__kvm_nvhe_kvm_init_params:
__kvm_nvhe_kvm_stacktrace_info:
__kvm_nvhe_overflow_stack:
__kvm_nvhe___per_cpu_end:
