#ifndef B1NIX_MEMTYPE_H
#define B1NIX_MEMTYPE_H

#include <b1nix/types.h>

/*
 * M98 T2 — memory typing (IA32_PAT) and cache-maintenance primitives.
 *
 * b1nix only ever mapped pages write-back (no PAT/PCD/PWT) or uncacheable
 * (PCD, via vmm_map_mmio). A GPU needs a third type: write-combining. A GTT or
 * a scanout buffer mapped UC is written one uncached store at a time, which is
 * one to two orders of magnitude slower than the WC write-combining buffers,
 * and the display engine does not snoop the LLC — so a WB framebuffer has to be
 * flushed to memory before the device reads it.
 *
 * The PAT is a per-CPU MSR holding eight memory types indexed by
 * (PAT << 2) | (PCD << 1) | PWT taken from the page-table entry. The reset
 * value is
 *
 *     PA0 WB   PA1 WT   PA2 UC-  PA3 UC   PA4 WB   PA5 WT   PA6 UC-  PA7 UC
 *
 * and b1nix rewrites exactly one slot: PA5 becomes write-combining. Slots 0-3
 * are the ones every existing mapping selects (the PTE PAT bit, bit 7, is
 * always clear today), so no live mapping changes meaning and the rewrite needs
 * no cache/TLB flush dance. VMM_WC is therefore PAT|PWT == index 5.
 */

#define IA32_MSR_PAT 0x277u

/* The eight-slot encoding programmed by pat_init_cpu(). */
#define B1NIX_PAT_VALUE 0x0007010600070406ULL

/* x86-only primitives: MSRs, fences and CLFLUSH have no AArch64 equivalent
 * (and every caller of the PAT/cache API below is already inside an x86 guard).
 * The header itself is included unconditionally, so gate the asm, not the
 * include. */
#if defined(__x86_64__)
static inline u64 rdmsr(u32 msr)
{
	u32 lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((u64)hi << 32) | lo;
}

static inline void wrmsr(u32 msr, u64 value)
{
	__asm__ volatile("wrmsr"
	                 :
	                 : "c"(msr), "a"((u32)(value & 0xffffffffu)),
	                   "d"((u32)(value >> 32)));
}

/* Full memory fence — orders WC stores against a subsequent doorbell write. */
static inline void mem_mfence(void)
{
	__asm__ volatile("mfence" ::: "memory");
}

static inline void mem_sfence(void)
{
	__asm__ volatile("sfence" ::: "memory");
}

/* Evict one cache line containing `addr` from every level of the hierarchy. */
static inline void mem_clflush(const void *addr)
{
	__asm__ volatile("clflush (%0)" : : "r"(addr) : "memory");
}

/* Write back and invalidate the whole cache hierarchy. Expensive; only for the
 * cases where a device rewrites a large region behind the CPU's back. */
static inline void mem_wbinvd(void)
{
	__asm__ volatile("wbinvd" ::: "memory");
}
#else
/* AArch64 barrier equivalents — the LKPI MMIO helpers (kernel/include/lkpi/
 * io.h) call these on every arch. */
static inline void mem_mfence(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void mem_sfence(void) { __asm__ volatile("dsb st" ::: "memory"); }
static inline void mem_clflush(const void *addr)
{
	__asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
}
static inline void mem_wbinvd(void) { __asm__ volatile("dsb sy" ::: "memory"); }
#endif

/* Program this CPU's IA32_PAT. Called once on the BSP from arch_init and once
 * per AP from x86_ap_arch_init — the PAT is per-CPU state and an AP that never
 * runs this would interpret a WC PTE as write-through. */
void pat_init_cpu(void);

/* 1 once the BSP has programmed the PAT (i.e. VMM_WC is meaningful). */
int pat_available(void);

/* Cache-line size reported by CPUID leaf 1 (CLFLUSH line size * 8), 64 when
 * the CPU does not report one. */
u32 cache_line_size(void);

/* clflush every line of [addr, addr+size), bracketed by fences. Safe to call on
 * any mapping; a no-op when the CPU lacks CLFLUSH. */
void cache_flush_range(const void *addr, usize size);

/* Same, with the choice forced. force_wbinvd != 0 takes the CLFLUSH-less
 * fallback (write back and invalidate the whole hierarchy) even on a CPU that
 * has CLFLUSH — which is the only way that branch is reachable on hardware
 * QEMU emulates, and how the M98 self-test exercises it. */
void cache_flush_range_ex(const void *addr, usize size, int force_wbinvd);

/* 1 when CPUID reported CLFLUSH, i.e. when cache_flush_range takes the
 * line-at-a-time path rather than the wbinvd fallback. */
int cache_have_clflush(void);

/* M98 in-kernel self-test: verifies the PAT MSR readback, that a WC mapping
 * carries the expected PTE bits, and that data written through it is coherent.
 * Emits M98-DRV-SMOKE markers. No-op outside b1nix.test=1. */
void memtype_selftest(void);

#endif
