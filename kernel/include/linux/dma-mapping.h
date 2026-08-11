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

/* The mask for a device that can address `n` bits. Written with the shift on
 * the far side so n == 64 does not shift a 64-bit value by 64, which is
 * undefined and in practice returns 1. */
#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))


#define DMA_ATTR_NO_WARN         (1ul << 8)
#define DMA_ATTR_NO_KERNEL_MAPPING (1ul << 2)

/* The attrs-taking spellings. b1nix's DMA layer has no attribute that changes
 * what a mapping is — see the bounce-buffer path from M99 — so the attributes
 * are recorded nowhere and the mapping is the ordinary one. */
#define dma_map_sg_attrs(dev, sg, nents, dir, attrs) \
	({ (void)(attrs); dma_map_sg(dev, sg, nents, dir); })
#define dma_unmap_sg_attrs(dev, sg, nents, dir, attrs) \
	do { (void)(attrs); dma_unmap_sg(dev, sg, nents, dir); } while (0)

/* The coherent-mask and max-segment limits a device declares. b1nix's IOMMU
 * path (M100) maps into a 64-bit address space and its bounce path allocates
 * below 4 GiB unconditionally, so a narrower mask is already satisfied and a
 * wider one changes nothing. The segment limit is likewise not consulted: the
 * scatterlist builder splits at page boundaries. Recording them would suggest
 * they are enforced. */
static inline int dma_set_coherent_mask(struct device *dev, u64 mask)
{ (void)dev; (void)mask; return 0; }
static inline int dma_set_max_seg_size(struct device *dev, unsigned int size)
{ (void)dev; (void)size; return 0; }


/*
 * Upstream's dma_map_sg() takes (dev, sgl, nents, dir); lkpi's takes the whole
 * sg_table, because that is what its bounce and IOMMU paths walk. The table is
 * rebuilt here from the two halves the caller passed — same entries, same
 * order. What is lost is the write-back of the mapped count into the caller's
 * table, which upstream also does not do: it returns it.
 */
#undef dma_map_sg
#undef dma_unmap_sg
#define dma_map_sg(dev_, sgl_, nents_, dir_)                              \
	({ (void)(dev_);                                                      \
	   struct sg_table __t = { .sgl = (sgl_), .nents = (nents_),           \
	                           .orig_nents = (nents_) };                   \
	   lkpi_dma_map_sg(&__t, (int)(dir_)); })
#define dma_unmap_sg(dev_, sgl_, nents_, dir_)                            \
	do { (void)(dev_);                                                    \
	     struct sg_table __t = { .sgl = (sgl_), .nents = (nents_),         \
	                             .orig_nents = (nents_) };                 \
	     lkpi_dma_unmap_sg(&__t, (int)(dir_)); } while (0)


/* Mapping a single page, and the coherent allocator in its attrs-taking
 * spelling. The attributes are accepted and not acted on — see the note on
 * dma_map_sg_attrs above. */
dma_addr_t dma_map_page(struct device *dev, struct page *page, usize offset,
                        usize size, enum dma_data_direction dir);
void dma_unmap_page(struct device *dev, dma_addr_t addr, usize size,
                    enum dma_data_direction dir);
static inline int dma_mapping_error(struct device *dev, dma_addr_t addr)
{ (void)dev; return addr == 0; }
#define dma_alloc_attrs(dev, size, handle, gfp, attrs) \
	({ (void)(attrs); dma_alloc_coherent(dev, size, handle, gfp); })
#define dma_free_attrs(dev, size, cpu, handle, attrs) \
	do { (void)(attrs); dma_free_coherent(dev, size, cpu, handle); } while (0)

#endif
