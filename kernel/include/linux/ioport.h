/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_IOPORT_H
#define LKPI_LINUX_IOPORT_H
#include <linux/types.h>
/* A claimed range of I/O or memory address space. b1nix does not arbitrate
 * these — one driver per device, assigned by PCI enumeration — so a request
 * always succeeds and there is no registry to conflict in. */
struct resource {
	u64 start;
	u64 end;
	const char *name;
	unsigned long flags;
	struct resource *parent, *sibling, *child;
};
#define IORESOURCE_MEM 0x00000200
#define IORESOURCE_IO  0x00000100
/* The root of the memory-resource tree. b1nix does not arbitrate ranges, so it
 * is an empty root that every request trivially fits in. */
extern struct resource iomem_resource;

static inline u64 resource_size(const struct resource *r)
{ return r->end - r->start + 1; }
#endif
