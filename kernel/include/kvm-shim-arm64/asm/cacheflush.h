/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_CACHEFLUSH_H
#define KVM_SHIM_ARM64_ASM_CACHEFLUSH_H
/*
 * arm64 cache maintenance by virtual-address range, as KVM calls it when it
 * maps guest memory whose stage-2 attributes differ from the host's (M131).
 * Upstream implements these in arch/arm64/mm/cache.S; b1nix's are in the KVM
 * glue, looping DC/IC by the line sizes CTR_EL0 reports.
 */
#include <linux/types.h>
#include <asm/barrier.h>
#include <asm/cpufeature.h>

void caches_clean_inval_pou(unsigned long start, unsigned long end);
void icache_inval_pou(unsigned long start, unsigned long end);
void dcache_clean_inval_poc(unsigned long start, unsigned long end);
void dcache_inval_poc(unsigned long start, unsigned long end);
void dcache_clean_poc(unsigned long start, unsigned long end);
void dcache_clean_pou(unsigned long start, unsigned long end);

static inline void flush_icache_range(unsigned long start, unsigned long end)
{
	caches_clean_inval_pou(start, end);
}

/* Invalidate every instruction cache to the point of unification, on all
 * CPUs of the inner-shareable domain. Nothing to do when CTR_EL0.DIC says the
 * instruction cache is coherent with data. */
static __always_inline void icache_inval_all_pou(void)
{
	if (alternative_has_cap_unlikely(ARM64_HAS_CACHE_DIC))
		return;

	asm("ic	ialluis");
	dsb(ish);
}
#endif
