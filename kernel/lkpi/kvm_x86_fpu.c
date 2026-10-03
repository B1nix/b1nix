// SPDX-License-Identifier: GPL-2.0-only
/*
 * A guest's FPU state on b1nix's task FPU model (M131).
 *
 * KVM gives each vCPU an fpstate of its own and swaps it in for KVM_RUN
 * (fpu_swap_kvm_fpstate) and out again when KVM_RUN returns. b1nix restores
 * a task's registers eagerly on every switch, from the XSAVE area the
 * scheduler keeps per task; swapping in therefore means: save the task's
 * registers into its own area, point the scheduler at the guest's image and
 * load it. A context switch in the middle of KVM_RUN then saves and restores
 * the guest's registers, which is exactly what Linux gets by swapping
 * fpu->fpstate. The images are standard-format XSAVE (b1nix uses XSAVE, not
 * XSAVES), which is also the uabi format KVM_GET/SET_XSAVE copy.
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/preempt.h>
#include <asm/fpu/types.h>
#include <asm/fpu/api.h>
#include <asm/fpu/xstate.h>
#include <b1nix/kvm_bridge.h>

/* A guest fpstate with the task area it displaced while loaded. The fpstate
 * ends in a 64-byte aligned XSAVE image; the box is allocated with slack so
 * that alignment holds wherever the allocator put it. */
struct kvm_fpbox {
	void *raw;
	void *host_area;
	struct fpstate fps;
};

struct fpu_state_config fpu_kernel_cfg, fpu_user_cfg;

static void fpu_cfg_init(void)
{
	u64 mask = b1nix_kvm_xsave_mask();
	unsigned int size = (unsigned int)b1nix_kvm_xsave_size();

	if (fpu_kernel_cfg.max_size)
		return;
	fpu_kernel_cfg.max_size = fpu_kernel_cfg.default_size = size;
	fpu_kernel_cfg.max_features = fpu_kernel_cfg.default_features = mask;
	fpu_kernel_cfg.independent_features = 0;
	fpu_user_cfg = fpu_kernel_cfg;
}

u64 xstate_get_guest_group_perm(void)
{
	fpu_cfg_init();
	return fpu_user_cfg.max_features;
}

bool fpu_alloc_guest_fpstate(struct fpu_guest *gfpu)
{
	unsigned int size;
	struct kvm_fpbox *box;
	void *raw;

	fpu_cfg_init();
	size = fpu_kernel_cfg.default_size;
	raw = kzalloc(sizeof(struct kvm_fpbox) + size + 64, GFP_KERNEL);
	if (!raw)
		return false;
	box = (struct kvm_fpbox *)(((unsigned long)raw + 63) & ~63UL);
	/* regs sits at the end of struct fpstate, aligned to 64 by its type;
	 * the box's own alignment keeps it so. */
	box->raw = raw;
	box->fps.size = size;
	box->fps.user_size = size;
	box->fps.xfeatures = fpu_kernel_cfg.default_features;
	box->fps.user_xfeatures = fpu_user_cfg.default_features;
	box->fps.is_guest = 1;
	/* All-zero header: every component in its init state, which is what
	 * XRSTOR loads for a fresh vCPU (the x87 control word and MXCSR too). */
	box->fps.regs.xsave.i387.cwd = 0x37f;
	box->fps.regs.xsave.i387.mxcsr = 0x1f80;
	gfpu->fpstate = &box->fps;
	gfpu->xfeatures = fpu_user_cfg.default_features;
	gfpu->uabi_size = size;
	gfpu->xfd_err = 0;
	return true;
}

void fpu_free_guest_fpstate(struct fpu_guest *gfpu)
{
	struct kvm_fpbox *box;

	if (!gfpu->fpstate)
		return;
	box = container_of(gfpu->fpstate, struct kvm_fpbox, fps);
	gfpu->fpstate = NULL;
	kfree(box->raw);
}

int fpu_swap_kvm_fpstate(struct fpu_guest *gfpu, bool enter_guest)
{
	struct kvm_fpbox *box = container_of(gfpu->fpstate, struct kvm_fpbox, fps);
	u64 mask = b1nix_kvm_xsave_mask();
	void *guest = &box->fps.regs;
	unsigned long flags;

	local_irq_save(flags);
	if (enter_guest) {
		void *host = b1nix_kvm_task_xsave_area();

		if (!host) {
			local_irq_restore(flags);
			return -ENOMEM;
		}
		b1nix_kvm_xsave(host, mask);
		box->host_area = host;
		b1nix_kvm_task_set_xsave_area(guest);
		b1nix_kvm_xrstor(guest, mask);
		box->fps.in_use = 1;
	} else if (box->host_area) {
		b1nix_kvm_xsave(guest, mask);
		b1nix_kvm_task_set_xsave_area(box->host_area);
		b1nix_kvm_xrstor(box->host_area, mask);
		box->host_area = NULL;
		box->fps.in_use = 0;
	}
	local_irq_restore(flags);
	return 0;
}

/* XFD (extended feature disable) exists only with AMX; without it there is
 * nothing for these to do. */
int fpu_enable_guest_xfd_features(struct fpu_guest *guest_fpu, u64 xfeatures)
{
	(void)guest_fpu; (void)xfeatures;
	return 0;
}

void fpu_update_guest_xfd(struct fpu_guest *guest_fpu, u64 xfd)
{
	(void)guest_fpu; (void)xfd;
}

void fpu_sync_guest_vmexit_xfd_state(void)
{
}

/* Where component nr lives in a standard-format XSAVE image, and its size. */
static bool xcomp(int nr, u32 *off, u32 *size)
{
	u32 a, b, c, d;

	if (nr < 2)
		return false;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			 : "a"(0xd), "c"((u32)nr));
	*size = a;
	*off = b;
	return a != 0;
}

void fpstate_clear_xstate_component(struct fpstate *fps, unsigned int xfeature)
{
	u32 off, size;

	fps->regs.xsave.header.xfeatures &= ~(1ULL << xfeature);
	if (xcomp((int)xfeature, &off, &size) && off + size <= fps->size)
		memset((u8 *)&fps->regs + off, 0, size);
}

void *get_xsave_addr(struct xregs_state *xsave, int xfeature_nr)
{
	u32 off, size;

	if (!(xsave->header.xfeatures & (1ULL << xfeature_nr)))
		return NULL;
	if (!xcomp(xfeature_nr, &off, &size))
		return NULL;
	return (u8 *)xsave + off;
}

/* KVM_GET_XSAVE: the guest image as userspace reads it -- the same standard
 * format, limited to the features asked for, with PKRU from KVM's copy. */
void fpu_copy_guest_fpstate_to_uabi(struct fpu_guest *gfpu, void *buf,
				    unsigned int size, u64 xfeatures, u32 pkru)
{
	struct fpstate *fps = gfpu->fpstate;
	struct xregs_state *x = buf;
	unsigned int n = size < fps->size ? size : fps->size;
	u32 off, sz;

	memset(buf, 0, size);
	memcpy(buf, &fps->regs, n);
	x->header.xfeatures &= xfeatures;
	x->header.xcomp_bv = 0;
	if ((xfeatures & (1ULL << XFEATURE_PKRU)) && xcomp(XFEATURE_PKRU, &off, &sz) &&
	    off + sizeof(u32) <= size) {
		memcpy((u8 *)buf + off, &pkru, sizeof(pkru));
		x->header.xfeatures |= 1ULL << XFEATURE_PKRU;
	}
}

/* KVM_SET_XSAVE: take userspace's image if every feature it names is one
 * the guest has, and hand PKRU back to KVM. */
int fpu_copy_uabi_to_guest_fpstate(struct fpu_guest *gfpu, const void *buf,
				   u64 xcr0, u32 *vpkru)
{
	struct fpstate *fps = gfpu->fpstate;
	const struct xregs_state *x = buf;
	u32 off, sz;

	if (x->header.xfeatures & ~xcr0)
		return -EINVAL;
	if (x->header.xcomp_bv)
		return -EINVAL;
	memcpy(&fps->regs, buf, fps->size);
	if (vpkru && (x->header.xfeatures & (1ULL << XFEATURE_PKRU)) &&
	    xcomp(XFEATURE_PKRU, &off, &sz))
		memcpy(vpkru, (const u8 *)buf + off, sizeof(*vpkru));
	return 0;
}
