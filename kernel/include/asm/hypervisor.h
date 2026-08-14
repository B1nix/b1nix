/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_HYPERVISOR_H
#define LKPI_ASM_HYPERVISOR_H
#include <asm/cpufeature.h>
#include <linux/types.h>

/*
 * Whether this kernel is running on bare metal or inside a virtual machine.
 *
 * It matters to a GPU driver for one specific reason. i915 asks
 * i915_run_as_guest() to decide whether an IOMMU is translating its DMA, and
 * it cannot tell directly: a passed-through device is behind the *host's*
 * IOMMU, which the guest cannot see. Upstream's answer is that a guest must
 * assume the host is enforcing VT-d, and it turns that into real behaviour —
 * intel_scanout_needs_vtd_wa() forces 256 KiB alignment on a scanout buffer so
 * the display engine's prefetch cannot run off the end of the object into an
 * unmapped page.
 *
 * Answering "bare metal" here therefore does not merely mislabel the machine:
 * it switches off that workaround, the display engine prefetches past the
 * framebuffer, and every frame raises a plane fault. That is exactly what
 * happened on the passed-through UHD 630 until this existed.
 *
 * The detection is CPUID leaf 1, ECX bit 31 — the bit every hypervisor sets and
 * no physical CPU does. b1nix does not identify *which* hypervisor, because
 * nothing here needs to: the only question asked is "is this native".
 */
enum x86_hypervisor_type {
	X86_HYPER_NATIVE = 0,
	X86_HYPER_VMWARE,
	X86_HYPER_MS_HYPERV,
	X86_HYPER_XEN_PV,
	X86_HYPER_XEN_HVM,
	X86_HYPER_KVM,
	X86_HYPER_JAILHOUSE,
	X86_HYPER_ACRN,
};

static inline enum x86_hypervisor_type lkpi_hypervisor_type(void)
{
	return lkpi_cpu_has(X86_FEATURE_HYPERVISOR) ? X86_HYPER_KVM
	                                            : X86_HYPER_NATIVE;
}

/*
 * Reported as KVM when virtualised. The distinction between hypervisors is not
 * available from CPUID's feature bit alone — it needs the vendor leaf at
 * 0x40000000 — and no caller here looks for a particular one, so claiming a
 * specific type would be inventing detail. What every caller actually tests is
 * whether the answer is X86_HYPER_NATIVE.
 */
static inline bool hypervisor_is_type(enum x86_hypervisor_type type)
{
	return lkpi_hypervisor_type() == type;
}

#endif
