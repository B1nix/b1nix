// SPDX-License-Identifier: GPL-2.0-only
/*
 * Where KVM's glue listens in the kernel (M131); see <b1nix/kvm_hooks.h>.
 * Built on every configuration: the call sites are in the scheduler, the
 * return-to-user paths and the TLB code, and without KVM each is a load and
 * a compare against a NULL table.
 */
#include <b1nix/kvm_hooks.h>
#include <b1nix/arch.h>
#include <b1nix/lapic.h>

static int this_cpu(void)
{
#if defined(__x86_64__)
	struct percpu *p = get_percpu();

	return p ? (int)p->cpu_id : 0;
#else
	return 0;
#endif
}

/* ── hooks ────────────────────────────────────────────────────────────── */

const struct b1nix_kvm_hooks *volatile g_kvm_hooks;
static volatile u8 g_urn_armed[MAX_CPUS];

void b1nix_kvm_set_hooks(const struct b1nix_kvm_hooks *hooks)
{
	g_kvm_hooks = hooks;
}

void b1nix_kvm_arm_user_return(void)
{
	g_urn_armed[this_cpu()] = 1;
}

void kvm_hook_return_to_user(void)
{
	const struct b1nix_kvm_hooks *h = g_kvm_hooks;
	int cpu;

	if (!h)
		return;
	cpu = this_cpu();
	if (!g_urn_armed[cpu])
		return;
	g_urn_armed[cpu] = 0;
	if (h->return_to_user)
		h->return_to_user();
}

void kvm_hook_mm_invalidate(u64 pml4, u64 start, u64 end)
{
	const struct b1nix_kvm_hooks *h = g_kvm_hooks;

	if (h && h->mm_invalidate)
		h->mm_invalidate(pml4, start, end, interrupts_enabled());
}
