/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_IO_H
#define LKPI_LINUX_IO_H
#include <lkpi/io.h>
#include <linux/types.h>
#include <linux/string.h>

/*
 * Copying to and from device memory.
 *
 * On x86 MMIO is ordinary loads and stores, so these are memcpy — but they stay
 * separate names because on another architecture they are not, and imported
 * code chose the spelling deliberately.
 */
static inline void memcpy_fromio(void *dst, const void *src, usize n)
{
	memcpy(dst, src, n);
}

static inline void memcpy_toio(void *dst, const void *src, usize n)
{
	memcpy(dst, src, n);
}

static inline void memset_io(void *dst, int c, usize n) { memset(dst, c, n); }

/* The iowriteN/ioreadN spelling of the same accesses readl/writel provide.
 * Upstream keeps both because the io* forms also work on port I/O; here every
 * caller is memory-mapped, so they are the same operations under two names. */
#define ioread8(addr)       readb(addr)
#define ioread16(addr)      readw(addr)
#define ioread32(addr)      readl(addr)
#define iowrite8(v, addr)   writeb(v, addr)
#define iowrite16(v, addr)  writew(v, addr)
#define iowrite32(v, addr)  writel(v, addr)
#define ioread32_rep(addr, buf, count) \
	do { u32 *__b = (buf); for (unsigned long __i = 0; __i < (count); __i++) \
	     __b[__i] = readl(addr); } while (0)


/* Whether write-combining is really available through the PAT — see
 * <asm/set_memory.h> for why the answer must not be optimistic. */
#include <asm/cpufeature.h>
#include <asm/set_memory.h>


/* The 64-bit MMIO accessors, as two 32-bit halves — see
 * <linux/io-64-nonatomic-lo-hi.h> for why the order is the device's choice and
 * not the CPU's. */
#define ioread64(addr)      lo_hi_readq(addr)
#define iowrite64(v, addr)  lo_hi_writeq(v, addr)


/* An errno carried in an __iomem pointer. */
#define IOMEM_ERR_PTR(err) (__force void __iomem *)ERR_PTR(err)

/*
 * Write-combining over a physical range, via MTRRs.
 *
 * b1nix programs PAT (M98) rather than MTRRs, and the write-combining a driver
 * wants comes from the page protection it maps with — pgprot_writecombine() in
 * <linux/mm.h> — not from a range register. So there is no MTRR to add: this
 * reports "no register was taken" (0), which is exactly what upstream returns
 * on a PAT-only system, and the matching del is a no-op.
 */
static inline int arch_phys_wc_add(unsigned long base, unsigned long size)
{ (void)base; (void)size; return 0; }
static inline void arch_phys_wc_del(int handle) { (void)handle; }


/* Port I/O. b1nix's own accessors, under the names imported code uses; the
 * legacy VGA registers are the only thing that reaches them here. */
#include <b1nix/io.h>


/* Map physical memory write-back cached. b1nix's plain ioremap is cached. */
#define ioremap_cache(addr, size) ioremap(addr, size)

/* Map physical memory into the kernel with a chosen cacheability. b1nix's
 * ioremap variants cover the same three modes, so these route to them. */
#define MEMREMAP_WB  (1 << 0)
#define MEMREMAP_WT  (1 << 1)
#define MEMREMAP_WC  (1 << 2)
static inline void *memremap(u64 offset, usize size, unsigned long flags)
{
	if (flags & MEMREMAP_WC)
		return ioremap_wc(offset, size);
	if (flags & MEMREMAP_WT)
		return ioremap_wc(offset, size);
	return ioremap_cache(offset, size);
}
static inline void memunmap(void *addr) { iounmap(addr); }



#endif
