/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_ASM_MEMORY_H
#define KVM_SHIM_ARM64_ASM_MEMORY_H
/*
 * The arm64 kernel's memory layout, as KVM names it -- b1nix's, not Linux's.
 * The direct map and the __pa/__va conversions come from <kvm_linux_extra.h>
 * over linuxkpi; the kernel image is addressed through the same helpers. The
 * memory-type indices are MAIR_EL1's slots as boot.S sets them (0 Device-
 * nGnRE, 1 Normal WB, 2 Normal NC, and 3 left 0x00, which is Device-nGnRnE).
 * The
 * stage-2 attribute encodings are the architecture's.
 */
#include <linux/const.h>
#include <linux/sizes.h>
#include <linux/types.h>

#define VA_BITS			(CONFIG_ARM64_VA_BITS)
#define vabits_actual		((u64)VA_BITS)

#define MT_NORMAL		1
#define MT_NORMAL_TAGGED	1
#define MT_NORMAL_NC		2
#define MT_DEVICE_nGnRnE	3
#define MT_DEVICE_nGnRE		0

#define MT_S2_NORMAL		0xf
#define MT_S2_NORMAL_NC		0x5
#define MT_S2_DEVICE_nGnRE	0x1
#define MT_S2_FWB_NORMAL	6
#define MT_S2_FWB_NORMAL_NC	5
#define MT_S2_FWB_DEVICE_nGnRE	1

#define OVERFLOW_STACK_SIZE	SZ_4K
#define NVHE_STACK_SHIFT	PAGE_SHIFT
#define NVHE_STACK_SIZE		(UL(1) << NVHE_STACK_SHIFT)
#define NVHE_STACKTRACE_SIZE	((OVERFLOW_STACK_SIZE + NVHE_STACK_SIZE) / 2 + sizeof(long))

#ifndef __ASSEMBLY__
unsigned long lkpi_virt_to_phys(const volatile void *va);
unsigned long lkpi_direct_map_base(void);
void *lkpi_phys_to_virt(u64 phys);
static inline phys_addr_t virt_to_phys(const volatile void *x)
{
	return (phys_addr_t)lkpi_virt_to_phys(x);
}
#define __pa_symbol(x)		virt_to_phys(x)
/* A kernel-image address for a physical one: b1nix maps the image inside the
 * direct map, so it is the same address. */
#define __phys_to_kimg(pa)	((unsigned long)lkpi_phys_to_virt(pa))
#define pfn_to_kaddr(pfn)	lkpi_phys_to_virt((u64)(pfn) << PAGE_SHIFT)
#define virt_addr_valid(x)	(lkpi_virt_to_phys((const void *)(x)) != 0)
#define __phys_to_pfn(pa)	((unsigned long)((pa) >> PAGE_SHIFT))
#define __pfn_to_phys(pfn)	((phys_addr_t)(pfn) << PAGE_SHIFT)
/* The linear-map alias of a kernel-image symbol: the image lives inside
 * b1nix's direct map, so it is the symbol itself. */
#define lm_alias(x)		((void *)(x))
/* The identity map's VA size, as the hyp VA layout sizes itself by it. */
#define IDMAP_VA_BITS		48
/* The kernel image runs where it was loaded: virtual minus physical is 0. */
#define kimage_voffset		0UL
/* Top of RAM as a direct-map address, and the lowest RAM address; the arm64
 * KVM glue reads both from b1nix's memory map. */
void *lkpi_high_memory(void);
#define high_memory		lkpi_high_memory()
phys_addr_t memblock_start_of_DRAM(void);
/* Is this frame RAM in the direct map (not device memory)? */
bool pfn_is_map_memory(unsigned long pfn);
static inline unsigned long kaslr_offset(void) { return 0; }
#endif
#endif
