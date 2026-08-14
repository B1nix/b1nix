/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_IO_H
#define LKPI_IO_H

#include <b1nix/memtype.h>
#include <lkpi/types.h>

/*
 * ioremap and MMIO accessors.
 *
 * ioremap() is vmm_map_mmio() with the memory type spelled out. b1nix's MMIO
 * window is never reclaimed, so iounmap() is a bookkeeping no-op — mapping a
 * BAR is a once-per-device operation and leaking the virtual range costs
 * nothing measurable, whereas a real unmap would need a VA allocator that
 * nothing else in the kernel currently wants.
 *
 * The accessors are volatile and individually ordered by the compiler; MMIO
 * on x86 is strongly ordered against other MMIO, so no explicit fences are
 * needed between register writes. A write that must be visible before a DMA
 * doorbell still needs mem_mfence() (see <b1nix/memtype.h>).
 */

/* Uncacheable device mapping — the default for registers. */
void *ioremap(u64 phys, usize size);
/* Alias for callers spelling out the memory type. */
static inline void *ioremap_uc(u64 phys, usize size) { return ioremap(phys, size); }
/* Write-combining mapping — for apertures written in bulk (GTT, framebuffers,
 * command rings). Requires the PAT (see M98); degrades to write-through on a
 * CPU without it. */
void *ioremap_wc(u64 phys, usize size);
/* Write-back mapping of real RAM (stolen memory, shared buffers). */
void *ioremap_wb(u64 phys, usize size);
void iounmap(void *addr);

static inline u8 readb(const volatile void *addr)
{
	return *(const volatile u8 *)addr;
}
static inline u16 readw(const volatile void *addr)
{
	return *(const volatile u16 *)addr;
}
static inline u32 readl(const volatile void *addr)
{
	return *(const volatile u32 *)addr;
}
static inline u64 readq(const volatile void *addr)
{
	return *(const volatile u64 *)addr;
}
static inline void writeb(u8 v, volatile void *addr)
{
	*(volatile u8 *)addr = v;
}
static inline void writew(u16 v, volatile void *addr)
{
	*(volatile u16 *)addr = v;
}
static inline void writel(u32 v, volatile void *addr)
{
	*(volatile u32 *)addr = v;
}
static inline void writeq(u64 v, volatile void *addr)
{
	*(volatile u64 *)addr = v;
}

/* Ordering helpers with the names driver code uses. */
static inline void wmb(void) { mem_sfence(); }
static inline void rmb(void) { __asm__ volatile("lfence" ::: "memory"); }
static inline void mb(void) { mem_mfence(); }

#endif
