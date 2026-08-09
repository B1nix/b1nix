/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DMA_MAPPING_H
#define LKPI_LINUX_DMA_MAPPING_H
#include <lkpi/dma-mapping.h>
#include <linux/device.h>
#include <linux/scatterlist.h>
/* Onto lkpi's dma-mapping (M99/M100a/M100b): bounce buffers when a device
 * cannot reach the memory, and translation through the IOMMU when one is
 * present, so a narrow mask means "map it low" rather than "copy it". */
/* Named enum, because imported code declares variables of this type rather
 * than passing the constants straight through. */
/*
 * A named enum, because imported code declares variables of this type rather
 * than only passing the constants through.
 *
 * lkpi's own dma-mapping already defines these as macros with its own values;
 * they are undefined here so the enumerators can carry Linux's, which is what
 * imported source and the uapi headers agree on. The two numbering schemes
 * never meet: lkpi's .c files include <lkpi/dma-mapping.h>, imported code
 * includes this.
 */
#undef DMA_BIDIRECTIONAL
#undef DMA_TO_DEVICE
#undef DMA_FROM_DEVICE
#undef DMA_NONE

enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2,
	DMA_NONE = 3,
};
/* Map a whole sg table for a device. Returns 0 or a negative errno — unlike
 * dma_map_sg, which returns the entry count, and the difference is why both
 * spellings exist. */
/* Skip the cache maintenance a map would normally do, for a buffer the caller
 * knows the CPU has not touched. Accepted and ignored: b1nix's dma-mapping
 * decides cache work from the direction, and doing it anyway is correct if
 * slower — whereas honouring the hint wrongly would corrupt data. */
#define DMA_ATTR_SKIP_CPU_SYNC     (1UL << 0)
#define DMA_ATTR_WRITE_COMBINE     (1UL << 1)
#define DMA_ATTR_NO_KERNEL_MAPPING (1UL << 2)
#define DMA_ATTR_FORCE_CONTIGUOUS  (1UL << 3)

/* Largest single mapping a device can be given. b1nix's bounce pool has a
 * block size, but a mapping that exceeds it is split rather than refused, so
 * there is no ceiling to report. */
static inline usize dma_max_mapping_size(struct device *dev)
{ (void)dev; return (usize)-1; }

static inline int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
                                  enum dma_data_direction dir, unsigned long attrs)
{
	(void)dev;
	(void)attrs;
	return dma_map_sg(sgt, (int)dir) ? 0 : -ENOMEM;
}

static inline void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt,
                                     enum dma_data_direction dir,
                                     unsigned long attrs)
{
	(void)dev;
	(void)attrs;
	dma_unmap_sg(sgt, (int)dir);
}

static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{ (void)dev; (void)mask; return 0; }
static inline int dma_set_mask(struct device *dev, u64 mask)
{ (void)dev; (void)mask; return 0; }
#endif
