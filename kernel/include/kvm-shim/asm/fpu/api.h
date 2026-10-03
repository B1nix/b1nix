/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_FPU_API_H
#define KVM_SHIM_ASM_FPU_API_H
/*
 * The guest FPU interface KVM uses (M131), on b1nix's task FPU model.
 *
 * b1nix restores a task's FPU state eagerly on every switch and keeps it in
 * a per-task XSAVE area. A guest's FPU state is an fpstate of its own; while
 * KVM_RUN has it loaded, the task's XSAVE area pointer is swapped to the
 * guest's image, so a context switch in the middle saves and restores the
 * guest's registers -- the same thing Linux does by swapping
 * fpu->fpstate. Implemented in kernel/lkpi/kvm_fpu.c.
 */
#include_next <asm/fpu/api.h>
#include <linux/types.h>
#include <asm/fpu/types.h>

/* Eager restore: a return to user never finds the registers unloaded. */
#define TIF_NEED_FPU_LOAD 14
static inline bool test_thread_flag(int flag) { (void)flag; return false; }
static inline void switch_fpu_return(void) { }
static inline void fpregs_assert_state_consistent(void) { }
#define fpregs_lock()   preempt_disable()
#define fpregs_unlock() preempt_enable()

extern struct fpu_state_config fpu_kernel_cfg, fpu_user_cfg;
extern u64 xstate_get_guest_group_perm(void);
extern void fpstate_clear_xstate_component(struct fpstate *fpstate, unsigned int xfeature);
extern bool fpu_alloc_guest_fpstate(struct fpu_guest *gfpu);
extern void fpu_free_guest_fpstate(struct fpu_guest *gfpu);
extern int fpu_swap_kvm_fpstate(struct fpu_guest *gfpu, bool enter_guest);
extern int fpu_enable_guest_xfd_features(struct fpu_guest *guest_fpu, u64 xfeatures);
extern void fpu_update_guest_xfd(struct fpu_guest *guest_fpu, u64 xfd);
extern void fpu_sync_guest_vmexit_xfd_state(void);
extern void fpu_copy_guest_fpstate_to_uabi(struct fpu_guest *gfpu, void *buf,
					   unsigned int size, u64 xfeatures, u32 pkru);
extern int fpu_copy_uabi_to_guest_fpstate(struct fpu_guest *gfpu, const void *buf,
					  u64 xcr0, u32 *vpkru);

static inline void fpstate_set_confidential(struct fpu_guest *gfpu)
{
	gfpu->fpstate->is_confidential = true;
}
static inline bool fpstate_is_confidential(struct fpu_guest *gfpu)
{
	return gfpu->fpstate->is_confidential;
}
#endif
