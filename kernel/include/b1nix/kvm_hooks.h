/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_KVM_HOOKS_H
#define B1NIX_KVM_HOOKS_H
/*
 * The points in the kernel where KVM's glue listens (M131), for the kernel's
 * own call sites; see <b1nix/kvm_bridge.h> for what each hook means. With
 * no KVM loaded every one of these is a load and a compare.
 */
#include <b1nix/kvm_bridge.h>

extern const struct b1nix_kvm_hooks *volatile g_kvm_hooks;

/* Every user address, whatever the paging depth. */
#define KVM_HOOK_USER_END 0x0000ffffffffffffULL

static inline void kvm_hook_sched_out(void *prev, void *next)
{
	const struct b1nix_kvm_hooks *h = g_kvm_hooks;

	if (h && h->sched_out)
		h->sched_out(prev, next);
}

static inline void kvm_hook_sched_in(void *task, int cpu)
{
	const struct b1nix_kvm_hooks *h = g_kvm_hooks;

	if (h && h->sched_in)
		h->sched_in(task, cpu);
}

/* Interrupts off, immediately before the return to ring 3. */
void kvm_hook_return_to_user(void);

void kvm_hook_mm_invalidate(u64 pml4, u64 start, u64 end);

static inline void kvm_hook_cpus_down(void)
{
	const struct b1nix_kvm_hooks *h = g_kvm_hooks;

	if (h && h->cpus_down)
		h->cpus_down();
}

static inline void kvm_hook_cpus_up(void)
{
	const struct b1nix_kvm_hooks *h = g_kvm_hooks;

	if (h && h->cpus_up)
		h->cpus_up();
}

static inline void kvm_hook_mm_release(u64 pml4)
{
	const struct b1nix_kvm_hooks *h = g_kvm_hooks;

	if (h && h->mm_release)
		h->mm_release(pml4);
}
#endif
