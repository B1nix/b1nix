/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_IO_MAPPING_H
#define LKPI_LINUX_IO_MAPPING_H
#include <linux/io.h>
#include <linux/types.h>
#include <lkpi/page.h>

/*
 * A window onto device memory, mapped one page at a time.
 *
 * i915 uses this for the aperture: a BAR far larger than anything worth mapping
 * permanently, from which it wants one page at a time with write-combining. On
 * Linux the per-page map is a fixmap slot with preemption disabled; b1nix has
 * no fixmap, so the whole region is mapped once with the requested attributes
 * and the per-page calls are offsets into it. The consequence is honest: the
 * address is valid until unmap rather than until the matching io_mapping_unmap,
 * and a caller relying on the narrow lifetime gets a wider one, never a shorter.
 */
struct io_mapping {
	resource_size_t base;
	unsigned long size;
	void __iomem *iomem;
	unsigned long prot;
};

struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size);
void io_mapping_free(struct io_mapping *iomap);
bool io_mapping_init_wc(struct io_mapping *iomap, resource_size_t base,
                        unsigned long size);
void io_mapping_fini(struct io_mapping *iomap);

static inline void __iomem *io_mapping_map_wc(struct io_mapping *mapping,
                                              unsigned long offset,
                                              unsigned long size)
{ (void)size; return mapping && mapping->iomem ? (u8 __iomem *)mapping->iomem + offset : 0; }
static inline void io_mapping_unmap(void __iomem *vaddr) { (void)vaddr; }

static inline void __iomem *io_mapping_map_local_wc(struct io_mapping *mapping,
                                                    unsigned long offset)
{ return io_mapping_map_wc(mapping, offset, PAGE_SIZE); }
static inline void io_mapping_unmap_local(void __iomem *vaddr) { (void)vaddr; }

static inline void __iomem *io_mapping_map_atomic_wc(struct io_mapping *mapping,
                                                     unsigned long offset)
{ return io_mapping_map_local_wc(mapping, offset); }
static inline void io_mapping_unmap_atomic(void __iomem *vaddr) { (void)vaddr; }
#endif
