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
/* `dev` and `gfp` are upstream's. b1nix has one coherent pool and one
 * allocator behaviour, so neither selects anything — both are in the signature
 * because every imported caller passes them. */
struct device;
void *dma_alloc_coherent(struct device *dev, usize size,
                         dma_addr_t *dma_handle, u32 gfp);
/* Same shape as the allocation: the device leads, as upstream has it. */
void dma_free_coherent(struct device *dev, usize size, void *cpu_addr,
                       dma_addr_t dma_handle);

/* Make an existing kernel buffer visible to the device. Returns the device
 * address, or 0 when the buffer is not backed by direct-mapped physical memory
 * (which is the only kind b1nix can hand to a device without bounce buffers). */
dma_addr_t dma_map_single(void *cpu_addr, usize size, int direction);
void dma_unmap_single(dma_addr_t handle, usize size, int direction);

/*
 * The same, for a device that cannot address all of memory.
 *
 * `dma_mask` is the highest address the device can put on the bus. When the
 * buffer already lies inside that window this behaves exactly like
 * dma_map_single. When it does not — and with no IOMMU there is nothing to
 * remap it with — the mapping is satisfied by a *bounce buffer*: frames
 * allocated below the mask, the caller's data copied in before the device
 * reads it and copied back out after the device writes it. That copy is the
 * whole point: the alternative is refusing the mapping, which leaves a driver
 * with a buffer it simply cannot use.
 *
 * The direction decides which copies happen: TO_DEVICE copies in, FROM_DEVICE
 * copies out, BIDIRECTIONAL both. dma_unmap_single and the two sync calls
 * recognise a bounced handle and do the right copy, so a driver that already
 * calls them correctly needs no other change.
 */
dma_addr_t dma_map_single_masked(void *cpu_addr, usize size, int direction,
                                 u64 dma_mask);

/*
 * The reserved bounce pool (M100a).
 *
 * Reserved at boot, before anything has fragmented the free lists, because a
 * bounce block must be physically contiguous and asking the general allocator
 * for one at map time makes a mapping's success depend on what every other
 * allocation did. `b1nix.bounce-pool=<KiB>` sizes it; 0 disables it and sends
 * every bounce to the allocator. A mask too narrow for the pool falls back to
 * the allocator regardless — the pool is the common case, not the only one.
 */
void dma_bounce_pool_init(void);

/* Pool extent, -1 when there is no pool. */
int dma_bounce_pool_range(u64 *base, u64 *end);

/* Frames in the pool, frames handed out, the high-water mark, and how many
 * mappings currently hold pool frames — so exhaustion reports as itself. */
void dma_bounce_pool_stats(usize *frames, usize *in_use, usize *peak,
                           usize *mappings);

/* Fault injection: refuse every block larger than one page, which is what a
 * fragmented system looks like from inside the bounce allocator. The only way
 * to reach the per-run sg fallback deliberately — under QEMU a contiguous run
 * is always available. Test use only. */
void dma_bounce_force_single_page(int on);

/* Blocks a bounced mapping is made of: 1 for a whole-table bounce, one per run
 * when no single block was available. 0 when the handle is not bounced. */
u32 dma_bounce_mapping_blocks(dma_addr_t handle);

/* 1 when `handle` came back from a mapping that had to bounce. Diagnostics and
 * tests; a driver has no reason to care. */
int dma_mapping_is_bounced(dma_addr_t handle);

/* Cache maintenance around a mapping that the CPU and device take turns using.
 * For a device that does not snoop (a display engine reading a scanout buffer),
 * these are what actually push the data out of the CPU's caches. */
void dma_sync_single_for_device(dma_addr_t handle, usize size, int direction);
void dma_sync_single_for_cpu(dma_addr_t handle, usize size, int direction);

/* Map every run of an sg table. Since device address == physical address, this
 * validates the runs and returns the entry count; it never coalesces further.
 * Returns the number of mapped entries, or 0 on failure. */
u32 lkpi_dma_map_sg(struct sg_table *sgt, int direction);
void lkpi_dma_unmap_sg(struct sg_table *sgt, int direction);
/* The names b1nix's own code uses. <linux/dma-mapping.h> redefines them as
 * macros in upstream's four-argument shape. */
#define dma_map_sg lkpi_dma_map_sg
#define dma_unmap_sg lkpi_dma_unmap_sg

/* sg mapping for a device with a narrow window. Each run that falls outside the
 * mask is bounced individually and its `dma_address` points at the bounce; runs
 * inside the window keep their own address. dma_unmap_sg undoes both kinds. */
u32 dma_map_sg_masked(struct sg_table *sgt, int direction, u64 dma_mask);

/*
 * A device that has its own address space (M100b).
 *
 * With an IOMMU in front of it a device no longer sees physical addresses, so a
 * narrow dma_mask stops meaning "copy the buffer somewhere it can reach" and
 * starts meaning "give it an address it can reach" — which is what these calls
 * do once dma_device_attach() has moved the function into a translated domain.
 * Without an IOMMU (or before attaching) they behave exactly like the masked
 * calls above, bounce buffers and all, so a driver can use one code path.
 */
struct dma_device {
	u8 bus, slot, func;
	u64 dma_mask;
	int translated; /* 1 once the IOMMU is mapping for this function */
};

/* Take the function out of the identity/pass-through domain. Returns 0 when the
 * device is now translated, -1 when there is no IOMMU (the struct is still
 * usable — mappings just go through the ordinary path). */
int dma_device_attach(struct dma_device *dev, u8 bus, u8 slot, u8 func,
                      u64 dma_mask);
void dma_device_detach(struct dma_device *dev);

dma_addr_t dma_map_single_dev(struct dma_device *dev, void *cpu_addr,
                              usize size, int direction);
void dma_unmap_single_dev(struct dma_device *dev, dma_addr_t handle,
                          usize size, int direction);
u32 dma_map_sg_dev(struct dma_device *dev, struct sg_table *sgt, int direction);
void dma_unmap_sg_dev(struct dma_device *dev, struct sg_table *sgt,
                      int direction);

/* Highest device address this kernel will hand out. Everything the frame
 * allocator returns lies inside the direct map, so this reports that ceiling
 * rather than a fixed 32/64-bit mask. */
u64 dma_addressable_limit(void);

#endif
