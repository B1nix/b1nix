/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM_SHIM_ARM64_LINUX_MEMBLOCK_H
#define KVM_SHIM_ARM64_LINUX_MEMBLOCK_H
/*
 * RAM as memblock describes it. b1nix has no memblock allocator; the arm64
 * KVM glue presents b1nix's memory map as regions and allocates from its
 * frame allocator. Protected KVM (an nVHE mode, not run here) is the one
 * user that sizes hyp memory by them.
 */
#include <linux/types.h>

struct memblock_region {
	phys_addr_t base;
	phys_addr_t size;
	unsigned long flags;
};

/* The i-th RAM region, NULL past the last. */
struct memblock_region *lkpi_memblock_region(unsigned int i);
#define for_each_mem_region(region)					\
	for (unsigned int __mbi = 0;					\
	     ((region) = lkpi_memblock_region(__mbi)) != NULL; __mbi++)
phys_addr_t memblock_phys_alloc(phys_addr_t size, phys_addr_t align);
#endif
