/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_FPU_XSTATE_H
#define KVM_SHIM_ASM_FPU_XSTATE_H
/*
 * XSAVE feature groups as KVM reasons about them (M131). The masks are
 * Linux's definitions (arch/x86/include/asm/fpu/xstate.h, the part that is
 * pure constants); what the host actually supports is b1nix's XCR0, which
 * lkpi publishes as fpu_kernel_cfg / fpu_user_cfg (kernel/lkpi/kvm_fpu.c).
 */
#include <linux/types.h>
#include <asm/fpu/types.h>
#include <asm/fpu/api.h>
#include <asm/user.h>
#define XFEATURE_MASK_EXTEND	(~(XFEATURE_MASK_FPSSE | (1ULL << 63)))

#define FXSAVE_SIZE	512

#define XSAVE_HDR_SIZE	    64
#define XSAVE_HDR_OFFSET    FXSAVE_SIZE

#define XSAVE_YMM_SIZE	    256
#define XSAVE_YMM_OFFSET    (XSAVE_HDR_SIZE + XSAVE_HDR_OFFSET)

#define XSAVE_ALIGNMENT     64

/* All currently supported user features */
#define XFEATURE_MASK_USER_SUPPORTED (XFEATURE_MASK_FP | \
				      XFEATURE_MASK_SSE | \
				      XFEATURE_MASK_YMM | \
				      XFEATURE_MASK_OPMASK | \
				      XFEATURE_MASK_ZMM_Hi256 | \
				      XFEATURE_MASK_Hi16_ZMM	 | \
				      XFEATURE_MASK_PKRU | \
				      XFEATURE_MASK_BNDREGS | \
				      XFEATURE_MASK_BNDCSR | \
				      XFEATURE_MASK_XTILE | \
				      XFEATURE_MASK_APX)

/*
 * Features which are restored when returning to user space.
 * PKRU is not restored on return to user space because PKRU
 * is switched eagerly in switch_to() and flush_thread()
 */
#define XFEATURE_MASK_USER_RESTORE	\
	(XFEATURE_MASK_USER_SUPPORTED & ~XFEATURE_MASK_PKRU)

/* Features which are dynamically enabled for a process on request */
#define XFEATURE_MASK_USER_DYNAMIC	XFEATURE_MASK_XTILE_DATA

/* Supervisor features which are enabled only in guest FPUs */
#define XFEATURE_MASK_GUEST_SUPERVISOR	XFEATURE_MASK_CET_KERNEL

/* All currently supported supervisor features */
#define XFEATURE_MASK_SUPERVISOR_SUPPORTED (XFEATURE_MASK_PASID | \
					    XFEATURE_MASK_CET_USER | \
					    XFEATURE_MASK_GUEST_SUPERVISOR)

/*
 * A supervisor state component may not always contain valuable information,
 * and its size may be huge. Saving/restoring such supervisor state components
 * at each context switch can cause high CPU and space overhead, which should
 * be avoided. Such supervisor state components should only be saved/restored
 * on demand. The on-demand supervisor features are set in this mask.
 *
 * Unlike the existing supported supervisor features, an independent supervisor
 * feature does not allocate a buffer in task->fpu, and the corresponding
 * supervisor state component cannot be saved/restored at each context switch.
 *
 * To support an independent supervisor feature, a developer should follow the
 * dos and don'ts as below:
 * - Do dynamically allocate a buffer for the supervisor state component.
 * - Do manually invoke the XSAVES/XRSTORS instruction to save/restore the
 *   state component to/from the buffer.
 * - Don't set the bit corresponding to the independent supervisor feature in
 *   IA32_XSS at run time, since it has been set at boot time.
 */
#define XFEATURE_MASK_INDEPENDENT (XFEATURE_MASK_LBR)

/*
 * Unsupported supervisor features. When a supervisor feature in this mask is
 * supported in the future, move it to the supported supervisor feature mask.
 */
#define XFEATURE_MASK_SUPERVISOR_UNSUPPORTED (XFEATURE_MASK_PT)

/* All supervisor states including supported and unsupported states. */
#define XFEATURE_MASK_SUPERVISOR_ALL (XFEATURE_MASK_SUPERVISOR_SUPPORTED | \
				      XFEATURE_MASK_INDEPENDENT | \
				      XFEATURE_MASK_SUPERVISOR_UNSUPPORTED)

/*
 * The feature mask required to restore FPU state:
 * - All user states which are not eagerly switched in switch_to()/exec()
 * - The suporvisor states
 */
#define XFEATURE_MASK_FPSTATE	(XFEATURE_MASK_USER_RESTORE | \
				 XFEATURE_MASK_SUPERVISOR_SUPPORTED)

/*
 * Features in this mask have space allocated in the signal frame, but may not
 * have that space initialized when the feature is in its init state.
 */
#define XFEATURE_MASK_SIGFRAME_INITOPT	(XFEATURE_MASK_XTILE | \
					 XFEATURE_MASK_USER_DYNAMIC)

/* The size an XSAVE image needs for these features, compacted or not. */
u32 xstate_required_size(u64 xstate_bv, bool compacted);
void *get_xsave_addr(struct xregs_state *xsave, int xfeature_nr);
#endif
