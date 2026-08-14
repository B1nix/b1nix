/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_ASM_CPUFEATURE_H
#define LKPI_ASM_CPUFEATURE_H
#include <linux/types.h>

/*
 * CPU feature tests.
 *
 * Upstream's static_cpu_has patches the branch out at boot so a hot path pays
 * nothing; this is a plain CPUID query, which is slower and gives the same
 * answer. What matters is that the answer is real: i915 chooses between a
 * clflush and a wbinvd on it, and a wrong "yes" leaves stale lines in front of
 * the GPU.
 *
 * Only the features imported code actually tests are defined, encoded as
 * (leaf, register, bit) so the query needs no table.
 */
#define LKPI_X86_FEATURE(leaf, reg, bit) (((leaf) << 8) | ((reg) << 5) | (bit))

/* CPUID leaf 1, EDX bit 19 / ECX bit 19. */
#define X86_FEATURE_CLFLUSH   LKPI_X86_FEATURE(1, 3, 19)
#define X86_FEATURE_SSE4_1    LKPI_X86_FEATURE(1, 2, 19)
#define X86_FEATURE_PAT       LKPI_X86_FEATURE(1, 3, 16)
#define X86_FEATURE_XMM4_1    X86_FEATURE_SSE4_1

int lkpi_cpu_has(u32 feature);

static inline bool static_cpu_has(u32 feature) { return lkpi_cpu_has(feature) != 0; }
static inline bool boot_cpu_has(u32 feature)   { return lkpi_cpu_has(feature) != 0; }
static inline bool cpu_has(u32 feature)        { return lkpi_cpu_has(feature) != 0; }

/* <linux/processor.h> includes THIS header for the feature tests, so it must
 * not be included back from here — the cycle leaves X86_FEATURE_* undefined in
 * whichever of the two a translation unit reaches first. Callers that need both
 * include <linux/processor.h>, which pulls this one in. */


/* CPUID leaf 1, ECX bit 31: set by every hypervisor, clear on bare metal. */
#define X86_FEATURE_HYPERVISOR LKPI_X86_FEATURE(1, 2, 31)


/* CPUID leaf 7 subleaf 0, EBX bit 23. The leaf is encoded in the same packed
 * form as the others here; see LKPI_X86_FEATURE above. */
#define X86_FEATURE_CLFLUSHOPT LKPI_X86_FEATURE(7, 1, 23)

#endif
