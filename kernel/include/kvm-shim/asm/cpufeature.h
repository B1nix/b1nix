/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ASM_CPUFEATURE_H
#define KVM_SHIM_ASM_CPUFEATURE_H
#ifndef __ASSEMBLY__
/*
 * CPU features in Linux's numbering (M131).
 *
 * KVM does not merely ask "does the CPU have X": it maps each capability word
 * back to the CPUID leaf and register it came from (reverse_cpuid.h) to build
 * the CPUID it shows a guest, so the words have to be Linux's words. lkpi
 * fills boot_cpu_data.x86_capability from CPUID in that layout at the first
 * use (kernel/lkpi/kvm_cpu.c); the Linux-defined synthetic bits it sets are
 * the ones whose meaning it can establish, and the rest stay clear.
 *
 * The rest of lkpi (i915) uses <asm/cpufeature.h> from kernel/include, which
 * encodes a feature as (leaf, register, bit); this header is only on KVM's
 * include path, ahead of that one.
 */
#include <linux/types.h>
#include <linux/bitops.h>
#include <asm/cpufeatures.h>
#include <asm/processor.h>

#define X86_CAP_WORDS (NCAPINTS + NBUGINTS)
#define MAX_CPU_FEATURES (NCAPINTS * 32)

/* Which CPUID leaf and register each capability word was read from, in
 * Linux's order; KVM's reverse_cpuid.h is written against these names. */
enum cpuid_leafs {
	CPUID_1_EDX = 0,
	CPUID_8000_0001_EDX,
	CPUID_8086_0001_EDX,
	CPUID_LNX_1,
	CPUID_1_ECX,
	CPUID_C000_0001_EDX,
	CPUID_8000_0001_ECX,
	CPUID_LNX_2,
	CPUID_LNX_3,
	CPUID_7_0_EBX,
	CPUID_D_1_EAX,
	CPUID_LNX_4,
	CPUID_7_1_EAX,
	CPUID_8000_0008_EBX,
	CPUID_6_EAX,
	CPUID_8000_000A_EDX,
	CPUID_7_ECX,
	CPUID_8000_0007_EBX,
	CPUID_7_EDX,
	CPUID_8000_001F_EAX,
	CPUID_8000_0021_EAX,
	CPUID_LNX_5,
	NR_CPUID_WORDS,
};

extern const char * const x86_cap_flags[NCAPINTS * 32];

static inline bool lkpi_x86_test_cap(const u32 *caps, unsigned int bit)
{
	return (caps[bit / 32] >> (bit % 32)) & 1;
}

#define cpu_has(c, bit)          lkpi_x86_test_cap((c)->x86_capability, (bit))
#define this_cpu_has(bit)        lkpi_x86_test_cap(boot_cpu_data.x86_capability, (bit))
#define boot_cpu_has(bit)        lkpi_x86_test_cap(boot_cpu_data.x86_capability, (bit))
#define static_cpu_has(bit)      boot_cpu_has(bit)
#define cpu_feature_enabled(bit) boot_cpu_has(bit)
#define boot_cpu_has_bug(bit)    boot_cpu_has(bit)
#define static_cpu_has_bug(bit)  boot_cpu_has(bit)
#define cpu_has_bug(c, bit)      cpu_has(c, bit)

#define set_cpu_cap(c, bit)   ((c)->x86_capability[(bit) / 32] |= 1u << ((bit) % 32))
#define clear_cpu_cap(c, bit) ((c)->x86_capability[(bit) / 32] &= ~(1u << ((bit) % 32)))
#define setup_force_cpu_cap(bit)   set_cpu_cap(&boot_cpu_data, bit)
#define setup_clear_cpu_cap(bit)   clear_cpu_cap(&boot_cpu_data, bit)

#endif
#endif
