/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_IO_64_NONATOMIC_LO_HI_H
#define LKPI_LINUX_IO_64_NONATOMIC_LO_HI_H
#include <linux/io.h>
#include <linux/types.h>

/*
 * A 64-bit register read as two 32-bit halves, low first.
 *
 * x86_64 can issue a single 64-bit MMIO access, but some device registers must
 * not be read that way — a counter that latches its high half when the low one
 * is read gives a torn value under a single 64-bit access, and the order is the
 * device's requirement, not the CPU's. Which is why upstream has this header at
 * all, and why it is written out here rather than folded into readq.
 */
static inline u64 lo_hi_readq(const volatile void __iomem *addr)
{
	const volatile u32 __iomem *p = addr;
	u32 low = readl(p);
	u32 high = readl(p + 1);

	return low | ((u64)high << 32);
}

static inline void lo_hi_writeq(u64 val, volatile void __iomem *addr)
{
	writel((u32)val, addr);
	writel((u32)(val >> 32), (volatile u32 __iomem *)addr + 1);
}

#define readq  lo_hi_readq
#define writeq lo_hi_writeq
#define readq_relaxed  lo_hi_readq
#define writeq_relaxed lo_hi_writeq
#endif
