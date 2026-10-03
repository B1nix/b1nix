/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_PROCESSOR_H
#define KVM_SHIM_ARM64_ASM_PROCESSOR_H
/*
 * The arm64 thread state as <asm/fpsimd.h> names it. b1nix keeps a thread's
 * FP/SIMD registers in its own task structure, not in a Linux thread_struct,
 * and KVM never reaches current->thread: only the inline helpers of
 * <asm/fpsimd.h> take one, so the struct carries the fields they read.
 * b1nix runs neither SVE nor SME in the host, so every vector length is 0.
 */
#include <linux/types.h>
#include <asm/ptrace.h>
#include <asm/spectre.h>

enum vec_type {
	ARM64_VEC_SVE = 0,
	ARM64_VEC_SME,
	ARM64_VEC_MAX,
};

enum fp_type {
	FP_STATE_CURRENT,	/* Save based on current task state. */
	FP_STATE_FPSIMD,
	FP_STATE_SVE,
};

struct thread_struct {
	struct {
		struct user_fpsimd_state fpsimd_state;
	} uw;
	enum fp_type		fp_type;
	unsigned int		fpsimd_cpu;
	void			*sve_state;
	void			*sme_state;
	unsigned int		vl[ARM64_VEC_MAX];
	unsigned int		vl_onexec[ARM64_VEC_MAX];
	u64			svcr;
};

static inline unsigned int thread_get_vl(struct thread_struct *thread,
					 enum vec_type type)
{
	return thread->vl[type];
}

static inline unsigned int thread_get_sve_vl(struct thread_struct *thread)
{
	return thread_get_vl(thread, ARM64_VEC_SVE);
}

static inline unsigned int thread_get_sme_vl(struct thread_struct *thread)
{
	return thread_get_vl(thread, ARM64_VEC_SME);
}

#endif
