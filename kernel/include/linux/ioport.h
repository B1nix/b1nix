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
	/* What the range is, for /proc/iomem's classification. b1nix does not
	 * classify ranges, so this is carried and not read. */
	unsigned long desc;
	struct resource *parent, *sibling, *child;
};
#define IORESOURCE_MEM 0x00000200
#define IORESOURCE_IO  0x00000100
/* The root of the memory-resource tree. b1nix does not arbitrate ranges, so it
 * is an empty root that every request trivially fits in. */
extern struct resource iomem_resource;

static inline u64 resource_size(const struct resource *r)
{ return r->end - r->start + 1; }

/* A statically described memory window, for a device whose ranges are known
 * without probing. `end` is inclusive here as everywhere else — see
 * pci_resource_len. */
#define DEFINE_RES_NAMED(_start, _size, _name, _flags)           \
	((struct resource){ .start = (_start),                        \
	  .end = (_start) + (_size) - 1,                              \
	  .name = (_name), .flags = (_flags) })
#define DEFINE_RES_MEM_NAMED(_start, _size, _name)               \
	DEFINE_RES_NAMED(_start, _size, _name, IORESOURCE_MEM)
#define DEFINE_RES_MEM(_start, _size) DEFINE_RES_MEM_NAMED(_start, _size, 0)
#define DEFINE_RES_IRQ_NAMED(_irq, _name)                        \
	DEFINE_RES_NAMED(_irq, 1, _name, IORESOURCE_IRQ)


#define IORESOURCE_IRQ    0x00000400
#define IORESOURCE_DMA    0x00000800
#define IORESOURCE_UNSET  0x20000000
#define IORESOURCE_PREFETCH 0x00002000
#define IORESOURCE_MEM_64 0x00100000


#define IORES_DESC_NONE 0

/* Is the outer resource's range a superset of the inner one's? */
static inline bool resource_contains(const struct resource *outer,
                                     const struct resource *inner)
{
	return outer->start <= inner->start && outer->end >= inner->end;
}

/* Give a claimed range back. b1nix's resource claims are recorded for
 * /proc/iomem and are not consulted for exclusion, so releasing one removes a
 * report and frees nothing that another claimant was waiting on. */
static inline int release_resource(struct resource *res) { (void)res; return 0; }

#endif
