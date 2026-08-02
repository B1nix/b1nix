/* SPDX-License-Identifier: MIT */
#ifndef LKPI_DMA_MAPPING_H
#define LKPI_DMA_MAPPING_H

#include <lkpi/scatterlist.h>
#include <lkpi/types.h>

/*
 * dma-mapping.
 *
 * b1nix has no IOMMU, so a device address is the physical address and mapping
 * is bookkeeping plus, where it matters, a cache flush. Drivers must still call
 * these: they are the single place an IOMMU would be inserted, and they are
 * where the CPU-side cache maintenance for a non-snooping device lives.
 *
 * dma_alloc_coherent returns memory that needs no explicit synchronisation:
 * frames from the physical allocator, addressed through the write-back direct
 * map, which is coherent with device DMA on every x86 platform b1nix targets.
 */

#define DMA_TO_DEVICE   1
#define DMA_FROM_DEVICE 2
#define DMA_BIDIRECTIONAL 3

/* Allocate `size` bytes of DMA-able memory. Returns the CPU pointer and stores
 * the device address in *dma_handle, or NULL on failure. Zeroed. */
void *dma_alloc_coherent(usize size, dma_addr_t *dma_handle);
void dma_free_coherent(usize size, void *cpu_addr, dma_addr_t dma_handle);

/* Make an existing kernel buffer visible to the device. Returns the device
 * address, or 0 when the buffer is not backed by direct-mapped physical memory
 * (which is the only kind b1nix can hand to a device without bounce buffers). */
dma_addr_t dma_map_single(void *cpu_addr, usize size, int direction);
void dma_unmap_single(dma_addr_t handle, usize size, int direction);

/* Cache maintenance around a mapping that the CPU and device take turns using.
 * For a device that does not snoop (a display engine reading a scanout buffer),
 * these are what actually push the data out of the CPU's caches. */
void dma_sync_single_for_device(dma_addr_t handle, usize size, int direction);
void dma_sync_single_for_cpu(dma_addr_t handle, usize size, int direction);

/* Map every run of an sg table. Since device address == physical address, this
 * validates the runs and returns the entry count; it never coalesces further.
 * Returns the number of mapped entries, or 0 on failure. */
u32 dma_map_sg(struct sg_table *sgt, int direction);
void dma_unmap_sg(struct sg_table *sgt, int direction);

/* Highest device address this kernel will hand out. Everything the frame
 * allocator returns lies inside the direct map, so this reports that ceiling
 * rather than a fixed 32/64-bit mask. */
u64 dma_addressable_limit(void);

#endif
